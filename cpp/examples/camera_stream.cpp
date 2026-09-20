// camera_stream -- stream a real webcam (or a video file, for testing
// without one) over ReLink as three topics, to demonstrate the
// practical tradeoffs between them:
//
//   - image_raw:        uncompressed pixel bytes (relink/image.hpp)
//   - image_compressed:  JPEG at a fixed quality (relink/image.hpp again --
//                        Image itself is not JPEG-specific, it carries
//                        whatever bytes you hand it)
//   - image_adaptive:    JPEG whose quality is chosen every frame by
//                        AdaptiveBitrateController (relink/adaptive_bitrate.hpp),
//                        carried by CompressedImage (relink/compressed_image.hpp)
//                        -- which additionally stamps a capture timestamp
//                        and the quality actually used onto every chunk,
//                        so the subscriber can report real end-to-end
//                        latency and see quality react to the scene with
//                        no side channel back to the publisher.
//
// REQUIRES OPENCV, WHICH IS NOT PART OF RELINK AND IS NOT INSTALLED FOR
// YOU. Install it yourself first, e.g.:
//   Ubuntu/Debian: sudo apt install libopencv-dev
// then build with: g++ ... $(pkg-config --cflags --libs opencv4) ...
//
// Why the raw/compressed split exists at all: a raw 640x480 BGR frame is
// ~920KB and even a JPEG-compressed frame is usually well over ReLink's
// ~1400-byte MTU budget -- ReLink intentionally does not fragment large
// messages transparently ("one message, one UDP datagram" is the whole
// design), so Image/CompressedImage chunk it into MTU-sized pieces for
// you and reassemble them on the other end, the documented way to send
// something bigger than one datagram.
//
// Why image_adaptive exists on top of image_compressed: a single fixed
// JPEG quality is always a compromise -- high enough to look good on a
// busy scene wastes bandwidth on a static one, low enough to be cheap
// on a static scene visibly smears a busy one. AdaptiveBitrateController
// blends how much the scene actually changed (mean abs diff of
// grayscale frames, this example's own choice of motion signal -- the
// controller itself doesn't care how you measure it) with how many
// bytes/sec are actually going out, smoothed so quality doesn't flicker
// frame to frame. See adaptive_bitrate.hpp for the full rationale.
//
// Tested end-to-end (real camera, real chunked pub/sub) at 320x240
// (166 raw chunks/frame) and 640x480 (664 raw chunks/frame) at ~5 FPS:
// image_raw/image_compressed/image_adaptive all delivered 100% across
// every resolution this test camera supports, in both C++ and Python,
// and again end to end against a real 1920x1080/30fps video file
// standing in for a camera (see --video-file below). That relies on
// RelinkNode requesting a 4MB socket send/receive buffer by default
// (see relink/include/relink/udp_transport.hpp) -- without it, a
// several-hundred-chunk burst can overflow the OS's default buffer
// (often ~212KB on Linux) faster than recv+dispatch can drain it,
// silently dropping the tail of the image (a dropped chunk drops the
// WHOLE image -- neither Image nor CompressedImage ever retransmits).
// Even with the larger buffer, prefer image_compressed/image_adaptive
// for anything resembling real-time video, especially over WiFi or a
// busier network than loopback: real packet loss on a real network
// still hits a several-hundred-chunk raw frame far harder than a
// 2-3-chunk compressed one. On your own machine with a real 1080p+
// webcam this same code will negotiate whatever resolution the
// hardware actually supports (cv::VideoCapture::set() is a request,
// not a guarantee) -- raw chunk counts scale directly with resolution.
//
// Build (one line -- OpenCV's linker flags must come AFTER the source
// file, or you'll get "undefined reference" errors from the linker):
//   g++ -std=c++17 -I relink/include -pthread examples/cpp/camera_stream.cpp -o camera_stream $(pkg-config --cflags --libs opencv4)
// Run:
//   ./camera_stream pub                          # opens /dev/video0, streams all three topics
//   ./camera_stream pub --video-file clip.mp4    # use a video file instead of a camera (loops
//                                                  # at the end) -- handy for testing without hardware
//   ./camera_stream sub                          # receives, writes latest frames to disk

#include "relink/relink.hpp"
#include "relink/adaptive_bitrate.hpp"
#include <opencv2/opencv.hpp>
#include <cstdio>
#include <cstring>
#include <vector>
#include <chrono>
#include <thread>
#include <fstream>

enum Topics : uint16_t {
    TOPIC_IMAGE_RAW = 500,
    TOPIC_IMAGE_COMPRESSED = 501,
    TOPIC_IMAGE_ADAPTIVE = 502,
};

constexpr int kFrameWidth = 320;
constexpr int kFrameHeight = 240;

// Adaptive quality bounds and the target bitrate ceiling used for the
// image_adaptive demo topic -- see AdaptiveBitrateController's own
// header for what each knob does.
constexpr int kAdaptiveQualityMin = 20;
constexpr int kAdaptiveQualityMax = 80;
constexpr double kAdaptiveMotionCeiling = 25.0;
constexpr double kAdaptiveTargetBitrateBps = 3'000'000.0; // 3 Mbps

static uint64_t now_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

static double now_sec() {
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void run_publisher(RelinkNode& node, const std::string& video_file) {
    // Discovery FIRST, camera SECOND: opening a real camera device has
    // meaningful, variable startup latency (V4L2/GStreamer init). Mode
    // B (multicast) discovery sends its "here I am" beacon burst once,
    // early, on startup -- if that burst is delayed behind camera init,
    // it can miss a subscriber's own already-finished burst window and
    // never learn its address at all. Starting discovery immediately
    // avoids coupling its timing to unrelated, slower hardware setup.
    node.advertise_image(TOPIC_IMAGE_RAW);
    node.advertise_image(TOPIC_IMAGE_COMPRESSED);
    node.advertise_compressed_image(TOPIC_IMAGE_ADAPTIVE);
    node.spin_once();

    bool is_file = !video_file.empty();
    // Force V4L2 explicitly for a real camera -- OpenCV's default
    // backend probing on some systems picks GStreamer, whose pipeline
    // can fail outright at resolution/fps combinations V4L2 handles
    // fine on the same camera. A video file doesn't need (or support)
    // that backend hint.
    cv::VideoCapture cap = is_file ? cv::VideoCapture(video_file) : cv::VideoCapture(0, cv::CAP_V4L2);
    if (!cap.isOpened()) {
        std::fprintf(stderr, "camera_stream: could not open %s\n",
                     is_file ? video_file.c_str() : "/dev/video0");
        return;
    }
    if (!is_file) {
        // A request, not a guarantee -- cv::VideoCapture::set() may be
        // silently ignored by a given camera/driver. A video file
        // doesn't support this at all, so every frame is explicitly
        // resized below regardless of source, which is what actually
        // guarantees the wire size the subscriber assumes.
        cap.set(cv::CAP_PROP_FRAME_WIDTH, kFrameWidth);
        cap.set(cv::CAP_PROP_FRAME_HEIGHT, kFrameHeight);
    }

    std::printf("camera_stream: publishing image_raw (topic %u), image_compressed (topic %u), "
                "and image_adaptive (topic %u)%s%s\n",
                TOPIC_IMAGE_RAW, TOPIC_IMAGE_COMPRESSED, TOPIC_IMAGE_ADAPTIVE,
                is_file ? " from " : "", is_file ? video_file.c_str() : "");

    relink::AdaptiveBitrateController::Config cfg;
    cfg.quality_min = kAdaptiveQualityMin;
    cfg.quality_max = kAdaptiveQualityMax;
    cfg.motion_ceiling = kAdaptiveMotionCeiling;
    cfg.target_bitrate_bps = kAdaptiveTargetBitrateBps;
    relink::AdaptiveBitrateController controller(cfg);
    cv::Mat prev_gray;

    uint32_t frame_id = 0;
    while (true) {
        cv::Mat frame;
        cap >> frame;
        if (frame.empty()) {
            if (is_file) {
                // End of file, not a transient camera hiccup -- loop.
                cap.set(cv::CAP_PROP_POS_FRAMES, 0);
            }
            continue;
        }
        cv::resize(frame, frame, cv::Size(kFrameWidth, kFrameHeight));

        // Raw: send the frame's own pixel bytes directly, no encoding.
        size_t raw_len = frame.total() * frame.elemSize();
        node.publish_image(TOPIC_IMAGE_RAW, frame.data, raw_len, frame_id);

        // Compressed: JPEG-encode first at a FIXED quality -- typically
        // 10-50x smaller, meaning far fewer chunks/packets for the same
        // picture, but no better or worse on a busy vs. static scene.
        std::vector<uchar> jpeg;
        cv::imencode(".jpg", frame, jpeg, {cv::IMWRITE_JPEG_QUALITY, 70});
        node.publish_image(TOPIC_IMAGE_COMPRESSED, jpeg.data(), jpeg.size(), frame_id);

        // Adaptive: JPEG-encode at a quality AdaptiveBitrateController
        // picks this frame, from how much the scene changed since the
        // last one and the actual bytes/sec recently sent.
        cv::Mat gray;
        cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
        double motion = 0.0;
        if (!prev_gray.empty()) {
            cv::Mat diff;
            cv::absdiff(gray, prev_gray, diff);
            motion = cv::mean(diff)[0];
        }
        prev_gray = gray;
        double t = now_sec();
        int quality = controller.next_quality(motion, t);
        uint64_t capture_ts = now_ns();
        std::vector<uchar> adaptive_jpeg;
        cv::imencode(".jpg", frame, adaptive_jpeg, {cv::IMWRITE_JPEG_QUALITY, quality});
        controller.record_sent(adaptive_jpeg.size(), t);
        node.publish_compressed_image(TOPIC_IMAGE_ADAPTIVE, adaptive_jpeg.data(), adaptive_jpeg.size(),
                                       static_cast<uint8_t>(quality), frame_id, capture_ts);

        std::printf("frame %u: raw=%zu bytes, compressed=%zu bytes, adaptive=%zu bytes "
                    "(motion=%.1f, quality=%d)\n",
                    frame_id, raw_len, jpeg.size(), adaptive_jpeg.size(), motion, quality);

        node.spin_once();
        ++frame_id;
        std::this_thread::sleep_for(std::chrono::milliseconds(200)); // ~5 FPS
    }
}

static void run_subscriber(RelinkNode& node) {
    node.subscribe_image(TOPIC_IMAGE_RAW, [](uint32_t id, const std::vector<uint8_t>& data) {
        size_t expected = size_t(kFrameWidth) * kFrameHeight * 3;
        if (data.size() != expected) {
            std::fprintf(stderr, "image_raw: frame %u dropped (%zu bytes, expected %zu)\n",
                         id, data.size(), expected);
            return;
        }
        // Raw BGR bytes at the publisher's known resolution -- re-encode
        // to JPEG purely so it's viewable as a normal image file.
        cv::Mat mat(kFrameHeight, kFrameWidth, CV_8UC3, const_cast<uint8_t*>(data.data()));
        cv::imwrite("latest_raw.jpg", mat);
        std::printf("image_raw: frame %u complete (%zu bytes) -> latest_raw.jpg\n", id, data.size());
    });

    node.subscribe_image(TOPIC_IMAGE_COMPRESSED, [](uint32_t id, const std::vector<uint8_t>& data) {
        std::ofstream f("latest_compressed.jpg", std::ios::binary);
        f.write(reinterpret_cast<const char*>(data.data()), data.size());
        std::printf("image_compressed: frame %u complete (%zu bytes) -> latest_compressed.jpg\n",
                    id, data.size());
    });

    node.subscribe_compressed_image(TOPIC_IMAGE_ADAPTIVE,
        [](uint32_t id, const std::vector<uint8_t>& data, uint64_t capture_timestamp_ns, uint8_t quality) {
            double latency_ms = (now_ns() - capture_timestamp_ns) / 1e6;
            std::ofstream f("latest_adaptive.jpg", std::ios::binary);
            f.write(reinterpret_cast<const char*>(data.data()), data.size());
            std::printf("image_adaptive: frame %u complete (%zu bytes, quality=%u, latency=%.1fms) "
                        "-> latest_adaptive.jpg\n", id, data.size(), quality, latency_ms);
        });

    std::printf("camera_stream: subscribed, writing latest_raw.jpg / latest_compressed.jpg / "
                "latest_adaptive.jpg to the current directory as frames complete\n");
    node.spin();
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s [pub|sub] [rlcore_ip] [--video-file PATH]\n", argv[0]);
        return 1;
    }
    std::string video_file;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--video-file") == 0 && i + 1 < argc) {
            video_file = argv[++i];
        }
    }
    RelinkNode node;
    std::string rlcore_ip;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--video-file") == 0) { ++i; continue; }
        rlcore_ip = argv[i];
        break;
    }
    if (!rlcore_ip.empty()) {
        node.set_rlcore.ip(rlcore_ip);
    } else {
        node.use_multicast_discovery();
    }

    if (std::strcmp(argv[1], "pub") == 0) run_publisher(node, video_file);
    else run_subscriber(node);
    return 0;
}

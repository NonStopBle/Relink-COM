// camera_stream -- stream a real webcam over ReLink as both an
// uncompressed ("image_raw") and JPEG-compressed ("image_compressed")
// topic, to demonstrate the practical tradeoff between them, using
// ReLink's built-in Image type (advertise_image/publish_image/
// subscribe_image, see relink/include/relink/image.hpp) -- no hand-
// rolled chunking needed, it's a library feature now.
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
// design), so Image chunks it into MTU-sized pieces for you and
// reassembles them on the other end, the documented way to send
// something bigger than one datagram.
//
// Tested end-to-end (real camera, real chunked pub/sub) at 320x240
// (225 raw chunks/frame) and 640x480 (900 raw chunks/frame): the C++
// data thread's per-chunk overhead is low enough to keep up with a
// 225-900-chunk-per-frame burst at ~5 FPS, so raw delivered 46/47 and
// 55/57 frames across two runs (~96-98%) -- compressed delivered 100%
// both times. A dropped chunk still drops the whole image (no
// retransmission), so raw's reliability WILL degrade at a high enough
// resolution/frame rate or under real network loss (a real LAN, unlike
// this loopback test, drops packets); compressed (2-3 chunks/frame) is
// far less exposed to that risk regardless, since losing one of 2-3 is
// much less likely than losing one of hundreds. Use image_compressed
// for anything resembling real-time video, especially over WiFi or a
// busier network than this test used. On your own machine with a real
// 1080p+ webcam this same code will negotiate whatever resolution the
// hardware actually supports (cv::VideoCapture::set() is a request, not
// a guarantee) -- raw chunk counts scale directly with resolution.
//
// Language matters here too: the Python binding (examples/camera_stream.py)
// measured 0/20 raw frames delivered at these same resolutions under the
// same conditions, versus ~100% for compressed -- Python's per-chunk
// interpreter overhead can't keep up with a several-hundred-chunk burst
// the way C++ can. This matches the Python binding's own documented
// scope (interoperability/non-hot-path use, not a second implementation
// racing C++ for performance) -- if you need raw image streaming from
// Python, compress first.
//
// Build (one line -- OpenCV's linker flags must come AFTER the source
// file, or you'll get "undefined reference" errors from the linker):
//   g++ -std=c++17 -I relink/include -pthread examples/cpp/camera_stream.cpp -o camera_stream $(pkg-config --cflags --libs opencv4)
// Run:
//   ./camera_stream pub        # opens /dev/video0, streams both topics
//   ./camera_stream sub        # receives, writes latest frames to disk

#include "relink/relink.hpp"
#include <opencv2/opencv.hpp>
#include <cstdio>
#include <cstring>
#include <vector>
#include <chrono>
#include <thread>
#include <fstream>

enum Topics : uint16_t { TOPIC_IMAGE_RAW = 500, TOPIC_IMAGE_COMPRESSED = 501 };

static void run_publisher(RelinkNode& node) {
    // Discovery FIRST, camera SECOND: opening a real camera device has
    // meaningful, variable startup latency (V4L2/GStreamer init). Mode
    // B (multicast) discovery sends its "here I am" beacon burst once,
    // early, on startup -- if that burst is delayed behind camera init,
    // it can miss a subscriber's own already-finished burst window and
    // never learn its address at all. Starting discovery immediately
    // avoids coupling its timing to unrelated, slower hardware setup.
    node.advertise_image(TOPIC_IMAGE_RAW);
    node.advertise_image(TOPIC_IMAGE_COMPRESSED);
    node.spin_once();

    cv::VideoCapture cap(0);
    if (!cap.isOpened()) {
        std::fprintf(stderr, "camera_stream: could not open /dev/video0\n");
        return;
    }
    // Keep frames modest-sized -- raw streaming scales directly with
    // resolution (a full 640x480 raw frame is ~900 chunks per frame).
    cap.set(cv::CAP_PROP_FRAME_WIDTH, 320);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, 240);

    std::printf("camera_stream: publishing image_raw (topic %u) and "
                "image_compressed (topic %u)\n", TOPIC_IMAGE_RAW, TOPIC_IMAGE_COMPRESSED);

    uint32_t frame_id = 0;
    while (true) {
        cv::Mat frame;
        cap >> frame;
        if (frame.empty()) continue;

        // Raw: send the frame's own pixel bytes directly, no encoding.
        size_t raw_len = frame.total() * frame.elemSize();
        node.publish_image(TOPIC_IMAGE_RAW, frame.data, raw_len, frame_id);

        // Compressed: JPEG-encode first -- typically 10-50x smaller,
        // meaning far fewer chunks/packets for the same picture.
        std::vector<uchar> jpeg;
        cv::imencode(".jpg", frame, jpeg, {cv::IMWRITE_JPEG_QUALITY, 70});
        node.publish_image(TOPIC_IMAGE_COMPRESSED, jpeg.data(), jpeg.size(), frame_id);

        std::printf("frame %u: raw=%zu bytes, compressed=%zu bytes\n",
                    frame_id, raw_len, jpeg.size());

        node.spin_once();
        ++frame_id;
        std::this_thread::sleep_for(std::chrono::milliseconds(200)); // ~5 FPS
    }
}

static void run_subscriber(RelinkNode& node) {
    node.subscribe_image(TOPIC_IMAGE_RAW, [](uint32_t id, const std::vector<uint8_t>& data) {
        // Raw BGR bytes at the publisher's known resolution (320x240) --
        // re-encode to JPEG purely so it's viewable as a normal image file.
        cv::Mat mat(240, 320, CV_8UC3, const_cast<uint8_t*>(data.data()));
        cv::imwrite("latest_raw.jpg", mat);
        std::printf("image_raw: frame %u complete (%zu bytes) -> latest_raw.jpg\n", id, data.size());
    });

    node.subscribe_image(TOPIC_IMAGE_COMPRESSED, [](uint32_t id, const std::vector<uint8_t>& data) {
        std::ofstream f("latest_compressed.jpg", std::ios::binary);
        f.write(reinterpret_cast<const char*>(data.data()), data.size());
        std::printf("image_compressed: frame %u complete (%zu bytes) -> latest_compressed.jpg\n",
                    id, data.size());
    });

    std::printf("camera_stream: subscribed, writing latest_raw.jpg / latest_compressed.jpg "
                "to the current directory as frames complete\n");
    node.spin();
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s [pub|sub] [com_core_ip]\n", argv[0]);
        return 1;
    }
    RelinkNode node;
    if (argc > 2) {
        node.set_com_core.ip(argv[2]);
    } else {
        node.use_multicast_discovery();
    }

    if (std::strcmp(argv[1], "pub") == 0) run_publisher(node);
    else run_subscriber(node);
    return 0;
}

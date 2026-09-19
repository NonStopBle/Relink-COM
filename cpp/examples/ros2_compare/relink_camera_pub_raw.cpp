// ReLink camera publisher, C++ counterpart to
// python/relink_py/examples/ros2_compare/relink_camera_pub_raw.py --
// used for a head-to-head comparison against the ROS2 (rclcpp) camera
// publisher (ros2_camera_pub.cpp, built via the ros2_compare colcon
// package). Publishes either raw BGR bytes or JPEG-compressed bytes
// (--compressed) at a configurable resolution and target rate.
//
// An 8-byte send-timestamp (double, seconds since epoch) is prepended
// to each frame's payload so the subscriber can measure end-to-end
// latency, same convention as the Python version.
//
// Requires OpenCV. Build:
//   g++ -std=c++17 -O2 -I ../../relink/include -pthread relink_camera_pub_raw.cpp -o relink_camera_pub_raw $(pkg-config --cflags --libs opencv4)
// Run:
//   ./relink_camera_pub_raw [rlcore_ip] [--width W] [--height H] [--fps N] [--compressed] [--quality Q] [--video PATH] [--h264]
//
// --video PATH reads frames from a video file instead of a live camera
// (looping at EOF) -- sidesteps a live webcam's exclusive-open
// semantics, useful for smoke tests / repeatable comparisons.
//
// --h264 GPU-encodes frames as H264 (via VAAPI through gst-launch-1.0,
// see gst_h264_codec.hpp) instead of raw/JPEG -- far lower bytes/frame
// than raw at high resolutions, and unlike a fresh JPEG re-encode every
// frame, a real video codec's inter-frame prediction is what actually
// gets bitrate down to interactive-streaming levels at 1080p+. Requires
// gst-launch-1.0 plus the VAAPI GStreamer plugin and a supported GPU
// (`gst-inspect-1.0 vaapih264enc` to check). Takes priority over
// --compressed if both are given.

#include "relink/relink.hpp"
#include "gst_h264_codec.hpp"
#include <opencv2/opencv.hpp>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>
#include <string>
#include <chrono>
#include <thread>

using namespace std::chrono;

static const char* TOPIC_CAMERA = "/compare/camera";

struct Args {
    std::string rlcore_ip;
    int width = -1, height = -1;  // -1 = not explicitly set by the user
    double fps = -1.0;
    bool compressed = false;
    int quality = 70;
    std::string video;
    bool h264 = false;
};

static Args parse_args(int argc, char** argv) {
    Args a;
    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--width") a.width = std::atoi(argv[++i]);
        else if (arg == "--height") a.height = std::atoi(argv[++i]);
        else if (arg == "--fps") a.fps = std::atof(argv[++i]);
        else if (arg == "--compressed") a.compressed = true;
        else if (arg == "--quality") a.quality = std::atoi(argv[++i]);
        else if (arg == "--video") a.video = argv[++i];
        else if (arg == "--h264") a.h264 = true;
        else pos.push_back(arg);
    }
    if (!pos.empty()) a.rlcore_ip = pos[0];
    return a;
}

static double now_sec() {
    return duration_cast<duration<double>>(system_clock::now().time_since_epoch()).count();
}

int main(int argc, char** argv) {
    Args args = parse_args(argc, argv);

    RelinkNode node;
    if (!args.rlcore_ip.empty()) node.set_rlcore.ip(args.rlcore_ip);
    else node.use_multicast_discovery();

    node.advertise_image(TOPIC_CAMERA);
    node.spin_once();

    // Force V4L2 explicitly for a live camera -- OpenCV's default
    // backend probing on this system picks GStreamer, whose pipeline
    // fails outright at some resolution/fps combinations this camera
    // otherwise supports fine. A video file uses normal file autodetection.
    cv::VideoCapture cap = args.video.empty() ? cv::VideoCapture(0, cv::CAP_V4L2)
                                               : cv::VideoCapture(args.video);
    if (!cap.isOpened()) {
        std::fprintf(stderr, "relink_camera_pub_raw: could not open %s\n",
                     args.video.empty() ? "/dev/video0" : args.video.c_str());
        return 1;
    }
    if (!args.video.empty()) {
        // Follow the file's own native resolution/fps unless the user
        // explicitly overrode one.
        if (args.width < 0) args.width = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
        if (args.height < 0) args.height = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
        if (args.fps < 0) {
            double v = cap.get(cv::CAP_PROP_FPS);
            args.fps = v > 0 ? v : 30.0;
        }
    } else {
        if (args.width < 0) args.width = 320;
        if (args.height < 0) args.height = 240;
        if (args.fps < 0) args.fps = 30.0;
        cap.set(cv::CAP_PROP_FRAME_WIDTH, args.width);
        cap.set(cv::CAP_PROP_FRAME_HEIGHT, args.height);
        cap.set(cv::CAP_PROP_FPS, args.fps);
    }

    std::string mode = args.h264 ? "GPU H264" : (args.compressed ? "JPEG" : "raw BGR");
    std::printf("relink_camera_pub_raw: publishing %s frames at %dx%d, target %.1f fps, from %s, on %s\n",
                mode.c_str(), args.width, args.height, args.fps,
                args.video.empty() ? "camera 0" : args.video.c_str(), TOPIC_CAMERA);

    const auto period = duration<double>(1.0 / args.fps);
    std::atomic<uint32_t> frame_id{0};
    std::atomic<uint64_t> sent_bytes{0};
    auto start = steady_clock::now();
    auto next_send = start;
    int consecutive_failures = 0;

    // --h264: encoding happens on a background thread (see
    // gst_h264_codec.hpp); pending_timestamps_ carries each pushed raw
    // frame's send-time to the matching encoded-frame callback in FIFO
    // order (vaapih264enc with no B-frames preserves input order).
    std::mutex ts_mutex;
    std::deque<double> pending_timestamps;
    std::unique_ptr<gst_h264::Encoder> encoder;
    if (args.h264) {
        encoder = std::make_unique<gst_h264::Encoder>(
            args.width, args.height, args.fps, /*gop=*/static_cast<int>(args.fps),
            [&](const uint8_t* data, size_t len) {
                double t;
                {
                    std::lock_guard<std::mutex> lock(ts_mutex);
                    if (pending_timestamps.empty()) return;  // shouldn't happen
                    t = pending_timestamps.front();
                    pending_timestamps.pop_front();
                }
                std::vector<uint8_t> payload(8 + len);
                std::memcpy(payload.data(), &t, 8);
                std::memcpy(payload.data() + 8, data, len);
                uint32_t fid = frame_id.fetch_add(1);
                node.publish_image(TOPIC_CAMERA, payload.data(), payload.size(), fid);
                uint64_t total = sent_bytes.fetch_add(payload.size()) + payload.size();
                if ((fid + 1) % 30 == 0) {
                    double elapsed = duration<double>(steady_clock::now() - start).count();
                    std::printf("relink: %u frames, %.1f fps, %.2f MB/s, %zu bytes/frame\n",
                                fid + 1, (fid + 1) / elapsed, total / elapsed / 1e6, payload.size());
                }
            });
    }

    std::vector<uint8_t> payload;

    while (true) {
        cv::Mat frame;
        cap >> frame;
        if (frame.empty()) {
            if (!args.video.empty()) {
                cap.set(cv::CAP_PROP_POS_FRAMES, 0);  // loop the file at EOF
                continue;
            }
            if (++consecutive_failures == 30) {
                std::fprintf(stderr, "relink_camera_pub_raw: camera reads failing repeatedly "
                             "(device busy/still initializing?), still retrying...\n");
            }
            std::this_thread::sleep_for(milliseconds(50));
            continue;
        }
        consecutive_failures = 0;

        if (frame.cols != args.width || frame.rows != args.height) {
            cv::resize(frame, frame, cv::Size(args.width, args.height));
        }

        if (args.h264) {
            {
                std::lock_guard<std::mutex> lock(ts_mutex);
                pending_timestamps.push_back(now_sec());
            }
            size_t raw_len = frame.total() * frame.elemSize();
            encoder->push_frame(frame.data, raw_len);
            node.spin_once();
        } else {
            payload.clear();
            payload.resize(8);
            double t = now_sec();
            std::memcpy(payload.data(), &t, 8);

            if (args.compressed) {
                std::vector<uchar> jpeg;
                cv::imencode(".jpg", frame, jpeg, {cv::IMWRITE_JPEG_QUALITY, args.quality});
                payload.insert(payload.end(), jpeg.begin(), jpeg.end());
            } else {
                size_t raw_len = frame.total() * frame.elemSize();
                payload.insert(payload.end(), frame.data, frame.data + raw_len);
            }

            uint32_t fid = frame_id.fetch_add(1);
            node.publish_image(TOPIC_CAMERA, payload.data(), payload.size(), fid);
            uint64_t total = sent_bytes.fetch_add(payload.size()) + payload.size();

            node.spin_once();
            if ((fid + 1) % 30 == 0) {
                double elapsed = duration<double>(steady_clock::now() - start).count();
                std::printf("relink: %u frames, %.1f fps, %.2f MB/s, %zu bytes/frame\n",
                            fid + 1, (fid + 1) / elapsed, total / elapsed / 1e6, payload.size());
            }
        }

        next_send += duration_cast<steady_clock::duration>(period);
        auto sleep_left = next_send - steady_clock::now();
        if (sleep_left > steady_clock::duration::zero()) std::this_thread::sleep_for(sleep_left);
    }
}

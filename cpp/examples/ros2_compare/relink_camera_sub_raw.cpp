// ReLink camera subscriber, C++ counterpart to relink_camera_pub_raw.cpp
// and python/relink_py/examples/ros2_compare/relink_camera_sub_raw.py.
// Displays the live feed in an OpenCV window and prints running
// latency/FPS/bandwidth stats in the same format as the ROS2 side, for
// direct comparison. Auto-decodes JPEG (--compressed), GPU H264
// (--h264, must match the publisher), or reshapes raw BGR bytes by
// --width/--height, matching whichever mode the publisher was run with.
//
// Requires OpenCV. Build:
//   g++ -std=c++17 -O2 -I ../../relink/include -pthread relink_camera_sub_raw.cpp -o relink_camera_sub_raw $(pkg-config --cflags --libs opencv4)
// Run:
//   ./relink_camera_sub_raw [rlcore_ip] [--width W] [--height H] [--compressed] [--h264]

#include "relink/relink.hpp"
#include "gst_h264_codec.hpp"
#include <opencv2/opencv.hpp>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <memory>
#include <vector>
#include <string>
#include <chrono>
#include <deque>
#include <mutex>
#include <numeric>

using namespace std::chrono;

static const char* TOPIC_CAMERA = "/compare/camera";

struct Args {
    std::string rlcore_ip;
    int width = 320, height = 240;
    bool compressed = false;
    bool h264 = false;
};

static Args parse_args(int argc, char** argv) {
    Args a;
    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--width") a.width = std::atoi(argv[++i]);
        else if (arg == "--height") a.height = std::atoi(argv[++i]);
        else if (arg == "--compressed") a.compressed = true;
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

    uint64_t recv_bytes = 0;
    uint32_t recv_count = 0;
    std::deque<double> latencies;
    std::mutex stats_mutex;
    auto start = steady_clock::now();

    // --h264: decode happens on a background thread (see
    // gst_h264_codec.hpp) -- imshow is called from that thread's
    // callback since it's the only thread that ever touches the
    // decoded-frame window.
    std::unique_ptr<gst_h264::Decoder> decoder;
    if (args.h264) {
        decoder = std::make_unique<gst_h264::Decoder>(
            args.width, args.height,
            [w = args.width, h = args.height](const uint8_t* data, size_t len) {
                (void)len;
                cv::Mat frame(h, w, CV_8UC3, const_cast<uint8_t*>(data));
                cv::imshow("ReLink camera", frame);
                cv::waitKey(1);
            });
    }

    node.subscribe_image(TOPIC_CAMERA, [&](uint32_t /*id*/, const std::vector<uint8_t>& data) {
        double send_time;
        std::memcpy(&send_time, data.data(), 8);
        double latency_ms = (now_sec() - send_time) * 1000.0;

        const uint8_t* body = data.data() + 8;
        size_t body_len = data.size() - 8;

        if (args.h264) {
            decoder->push_nal(body, body_len);
        } else {
            cv::Mat frame;
            if (args.compressed) {
                std::vector<uint8_t> jpeg(body, body + body_len);
                frame = cv::imdecode(jpeg, cv::IMREAD_COLOR);
            } else {
                frame = cv::Mat(args.height, args.width, CV_8UC3, const_cast<uint8_t*>(body)).clone();
            }
            cv::imshow("ReLink camera", frame);
            cv::waitKey(1);
        }

        std::lock_guard<std::mutex> lock(stats_mutex);
        latencies.push_back(latency_ms);
        if (latencies.size() > 30) latencies.pop_front();
        recv_bytes += data.size();
        ++recv_count;
        if (recv_count % 30 == 0) {
            double elapsed = duration<double>(steady_clock::now() - start).count();
            double avg_lat = std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();
            std::printf("relink: %u frames, %.1f fps, %.2f MB/s, avg latency %.2f ms, %zu bytes/frame\n",
                        recv_count, recv_count / elapsed, recv_bytes / elapsed / 1e6, avg_lat, data.size());
        }
    });

    std::printf("relink_camera_sub_raw: subscribed to %s, displaying live (ctrl-c to quit)\n", TOPIC_CAMERA);
    node.spin();
}

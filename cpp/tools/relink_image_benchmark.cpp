// relink_image_benchmark -- head-to-head throughput/delivery comparison
// of raw, uncompressed full-HD image frames sent two ways:
//
//   - udp:  publish_image()/subscribe_image()          (relink/relink.hpp)
//   - shm:  publish_local_ipc_image()/subscribe_local_ipc_image()
//           (same-host shared-memory ring, relink/shm_transport.hpp)
//
// This is USAGE code exercising the public API, same as relink_benchmark
// and camera_stream -- not the library implementation itself.
//
// A raw 1920x1080 BGR8 frame is ~6.2MB, chunked into ~4,450 UDP
// datagrams by publish_image() -- losing any ONE of those chunks drops
// the whole frame (no retransmission, see subscribe_image()'s doc
// comment). The shared-memory ring has no MTU and no chunking at all:
// a whole frame is one ring slot. That difference is the entire point
// of this benchmark -- see the main README's Step 12 (Benchmarks) and
// Step 15's "Same-host shared-memory IPC" deep dive for measured
// numbers and the full explanation.
//
// REQUIRES OPENCV, WHICH IS NOT PART OF RELINK AND IS NOT INSTALLED FOR
// YOU. Install it yourself first, e.g.:
//   Ubuntu/Debian: sudo apt install libopencv-dev
//
// Build: this target is added automatically by cpp/CMakeLists.txt when
// OpenCV is found (same as camera_stream), or by hand:
//   g++ -std=c++17 -I cpp/include -pthread tools/relink_image_benchmark.cpp \
//       -o relink_image_benchmark $(pkg-config --cflags --libs opencv4) -lrt
//
// Run as two processes, e.g.:
//   ./relink_image_benchmark pub shm
//   ./relink_image_benchmark sub shm
//
//   ./relink_image_benchmark pub udp
//   ./relink_image_benchmark sub udp
//
// IMPORTANT for `udp`: start the PUBLISHER first, then the subscriber a
// moment later (~0.3-1s is enough). Multicast discovery sends a
// one-shot startup burst (3 beacons within ~400ms) and then goes quiet
// for 30-60s -- whichever side's listener isn't running yet when the
// OTHER side's burst goes out won't learn about it until the next
// re-announce, which is well past this benchmark's run time. This
// doesn't apply to `shm`: there's no peer to discover, subscribing
// attaches to the ring directly.
//
// Defaults to a real webcam (device 0); use --video-file PATH to drive
// it from a video file instead (loops at the end), same convention as
// camera_stream. --duration SEC controls the publisher's run length
// (default 8s); the subscriber always listens a couple seconds longer
// to catch the tail.

#include "relink/relink.hpp"
#include <opencv2/opencv.hpp>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <atomic>
#include <thread>
#include <chrono>

namespace {

constexpr uint32_t kTopicUdp = 800;
constexpr uint32_t kTopicShm = 801;
constexpr int kFrameW = 1920;
constexpr int kFrameH = 1080;
constexpr size_t kExpectedLen = size_t(kFrameW) * kFrameH * 3;

double now_sec() {
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void run_pub(const std::string& transport, double duration_sec, const std::string& video_file) {
    RelinkNode node;
    bool shm = (transport == "shm");

    if (shm) {
        if (!node.advertise_local_ipc_image(kTopicShm)) {
            std::fprintf(stderr, "advertise_local_ipc_image() failed\n");
            std::exit(1);
        }
    } else {
        node.use_multicast_discovery();
        node.advertise_image(kTopicUdp);
        node.spin_once();
    }

    cv::VideoCapture cap = video_file.empty() ? cv::VideoCapture(0, cv::CAP_V4L2)
                                                : cv::VideoCapture(video_file);
    if (!cap.isOpened()) {
        std::fprintf(stderr, "could not open %s\n",
                      video_file.empty() ? "camera device 0" : video_file.c_str());
        std::exit(1);
    }

    uint32_t frame_id = 0, pushed = 0, full_drops = 0;
    size_t bytes_total = 0;
    double start = now_sec();
    double deadline = start + duration_sec;
    while (now_sec() < deadline) {
        cv::Mat frame;
        cap >> frame;
        if (frame.empty()) {
            if (!video_file.empty()) { cap.set(cv::CAP_PROP_POS_FRAMES, 0); continue; }
            break; // real camera returning nothing is a real failure, not "loop"
        }
        if (frame.cols != kFrameW || frame.rows != kFrameH)
            cv::resize(frame, frame, cv::Size(kFrameW, kFrameH));

        size_t len = frame.total() * frame.elemSize();
        bytes_total += len;
        if (shm) {
            if (node.publish_local_ipc(kTopicShm, frame.data, len)) ++pushed;
            else ++full_drops;
        } else {
            node.publish_image(kTopicUdp, frame.data, len, frame_id);
            node.spin_once();
            ++pushed;
        }
        ++frame_id;
    }
    double elapsed = now_sec() - start;
    if (shm) node.request_stop();

    std::printf("PUB_SUMMARY transport=%s frames=%u pushed=%u full_drops=%u "
                "elapsed=%.2fs fps=%.1f throughput_MBps=%.1f\n",
                transport.c_str(), frame_id, pushed, full_drops, elapsed,
                pushed / elapsed, bytes_total / elapsed / 1e6);
}

void run_sub(const std::string& transport, double duration_sec) {
    RelinkNode node;
    bool shm = (transport == "shm");
    std::atomic<uint64_t> received{0}, wrong_size{0};

    if (shm) {
        if (!node.subscribe_local_ipc_image(kTopicShm, [&](const uint8_t*, size_t len) {
                if (len != kExpectedLen) wrong_size++;
                received++;
            })) {
            std::fprintf(stderr, "subscribe_local_ipc_image() failed\n");
            std::exit(1);
        }
    } else {
        node.use_multicast_discovery();
        node.subscribe_image(kTopicUdp, [&](uint32_t, const std::vector<uint8_t>& data) {
            if (data.size() != kExpectedLen) wrong_size++;
            received++;
        });
        node.spin_once(); // subscribe_image() alone never calls ensure_started()
    }

    std::this_thread::sleep_for(std::chrono::duration<double>(duration_sec));
    node.request_stop();

    std::printf("SUB_SUMMARY transport=%s received=%llu wrong_size=%llu "
                "elapsed=%.2fs fps=%.1f\n",
                transport.c_str(), (unsigned long long)received.load(),
                (unsigned long long)wrong_size.load(), duration_sec,
                received.load() / duration_sec);
}

void print_usage(const char* argv0) {
    std::fprintf(stderr,
        "usage: %s <pub|sub> <udp|shm> [--duration SEC] [--video-file PATH]\n\n"
        "Raw full-HD (1920x1080 BGR8) image throughput/delivery benchmark:\n"
        "the UDP-socket transport (publish_image/subscribe_image) vs the\n"
        "same-host shared-memory IPC ring (publish_local_ipc_image/\n"
        "subscribe_local_ipc_image). See the main README's Step 12\n"
        "(Benchmarks) for measured numbers and Step 15's same-host\n"
        "shared-memory IPC deep dive for why they differ this much.\n\n"
        "For --transport udp: start the PUBLISHER process first, then the\n"
        "subscriber ~0.3-1s later -- see the file header comment for why.\n",
        argv0);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) { print_usage(argv[0]); return 1; }

    std::string mode = argv[1];
    std::string transport = argv[2];
    if (mode != "pub" && mode != "sub") { print_usage(argv[0]); return 1; }
    if (transport != "udp" && transport != "shm") { print_usage(argv[0]); return 1; }

    double duration_sec = 8.0;
    std::string video_file;
    for (int i = 3; i < argc; ++i) {
        if (std::strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            duration_sec = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "--video-file") == 0 && i + 1 < argc) {
            video_file = argv[++i];
        } else {
            std::fprintf(stderr, "unrecognized argument: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (mode == "pub") {
        run_pub(transport, duration_sec, video_file);
    } else {
        run_sub(transport, duration_sec + 2.0); // margin to catch the publisher's tail
    }
    return 0;
}

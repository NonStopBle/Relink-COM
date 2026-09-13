// ROS2 comparison benchmark -- subscriber side. Mirrors relink_benchmark.cpp's
// exact latency measurement and reporting (best/avg/p50/p99/worst,
// 1ms budget PASS/FAIL) for a like-for-like comparison.

#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/u_int8_multi_array.hpp>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <vector>
#include <algorithm>
#include <csignal>
#include <atomic>
#include <thread>

using namespace std::chrono;

static uint64_t now_us() {
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

static std::atomic<bool> g_stop{false};
static void on_sigint(int) { g_stop = true; }

int main(int argc, char** argv) {
    std::signal(SIGINT, on_sigint);
    rclcpp::init(argc, argv);
    auto node = rclcpp::Node::make_shared("relink_bench_compare_sub");

    std::vector<double> latencies_us;
    latencies_us.reserve(70000);
    uint64_t first_recv_time = 0;
    uint64_t last_recv_time = 0;

    // Deep queue depth so the 1000Hz publish rate never overflows the
    // subscription queue and silently drops messages before the callback
    // even runs -- keeps this a fair latency comparison, not an
    // artificial-loss measurement.
    auto sub = node->create_subscription<std_msgs::msg::UInt8MultiArray>(
        "bench_topic", rclcpp::QoS(rclcpp::KeepLast(2000)),
        [&](const std_msgs::msg::UInt8MultiArray::SharedPtr msg) {
            uint64_t recv_time = now_us();
            uint64_t send_time = 0;
            std::memcpy(&send_time, msg->data.data(), sizeof(send_time));
            latencies_us.push_back(double(recv_time - send_time));
            if (first_recv_time == 0) first_recv_time = recv_time;
            last_recv_time = recv_time;
        });

    std::printf("subscriber: listening on bench_topic, press Ctrl+C when the publisher finishes...\n");

    // Proper blocking spin (not spin_some()+sleep polling, which caps
    // dispatch responsiveness to the sleep interval and was the actual
    // cause of message loss in the first run) -- callbacks run as soon
    // as a message is ready, same as ReLink's inline-on-data-thread
    // dispatch model.
    rclcpp::executors::SingleThreadedExecutor executor;
    executor.add_node(node);
    std::thread stopper([&] {
        while (!g_stop) std::this_thread::sleep_for(milliseconds(20));
        rclcpp::shutdown();
    });
    executor.spin();
    stopper.join();

    if (latencies_us.empty()) {
        std::printf("subscriber: no messages received\n");
        rclcpp::shutdown();
        return 0;
    }

    std::sort(latencies_us.begin(), latencies_us.end());
    size_t n = latencies_us.size();
    double p50 = latencies_us[n * 50 / 100];
    double p99 = latencies_us[n * 99 / 100];
    double worst = latencies_us[n - 1];
    double best = latencies_us[0];
    double sum = 0;
    for (double v : latencies_us) sum += v;
    double avg = sum / n;

    std::printf("\n--- results (%zu messages) ---\n", n);
    std::printf("best:   %8.1f us\n", best);
    std::printf("avg:    %8.1f us\n", avg);
    std::printf("p50:    %8.1f us\n", p50);
    std::printf("p99:    %8.1f us\n", p99);
    std::printf("worst:  %8.1f us\n", worst);

    constexpr double BUDGET_US = 1000.0;
    if (worst <= BUDGET_US) {
        std::printf("\nPASS: worst-case %.1fus is within the %.0fus budget\n", worst, BUDGET_US);
    } else {
        std::printf("\nFAIL: worst-case %.1fus exceeds the %.0fus budget\n", worst, BUDGET_US);
    }

    // Actual received rate from message count over wall-clock span, not
    // 1e6/avg_latency -- see relink_benchmark.cpp's run_subscriber() for
    // why that inverted-latency metric is a different, misleading
    // quantity, not an actual measured rate.
    double span_sec = double(last_recv_time - first_recv_time) / 1'000'000.0;
    if (span_sec > 0.0 && n > 1) {
        double received_hz = double(n - 1) / span_sec;
        std::printf("received rate: ~%.0f Hz (%zu messages over %.1fs)\n", received_hz, n, span_sec);
    }

    rclcpp::shutdown();
    return 0;
}

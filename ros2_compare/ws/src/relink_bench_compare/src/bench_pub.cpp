// ROS2 comparison benchmark -- publisher side. Mirrors relink_benchmark.cpp
// exactly: 1000Hz, 60s sustained, ~40-byte payload (8-byte timestamp +
// 32-byte padding), for a fair apples-to-apples comparison against the
// ReLink benchmark result.

#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/u_int8_multi_array.hpp>
#include <chrono>
#include <cstring>
#include <cstdio>

using namespace std::chrono;

static uint64_t now_us() {
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

int main(int argc, char** argv) {
    rclcpp::init(argc, argv);
    auto node = rclcpp::Node::make_shared("relink_bench_compare_pub");
    auto pub = node->create_publisher<std_msgs::msg::UInt8MultiArray>(
        "bench_topic", rclcpp::QoS(rclcpp::KeepLast(2000)));

    constexpr int DURATION_SEC = 60;
    constexpr int RATE_HZ = 1000;
    const auto period = microseconds(1000000 / RATE_HZ);

    std::printf("publisher: sending at %d Hz for %d seconds...\n", RATE_HZ, DURATION_SEC);

    auto start = steady_clock::now();
    auto next_send = start;
    uint64_t sent = 0;

    std_msgs::msg::UInt8MultiArray msg;
    msg.data.resize(40, 0); // 8B timestamp + 32B padding, matches BenchMsg size

    while (rclcpp::ok()) {
        auto elapsed = steady_clock::now() - start;
        if (elapsed > seconds(DURATION_SEC)) break;

        uint64_t t = now_us();
        std::memcpy(msg.data.data(), &t, sizeof(t));
        pub->publish(msg);
        ++sent;

        next_send += period;
        std::this_thread::sleep_until(next_send);
    }

    std::printf("publisher: done, sent %llu messages\n", (unsigned long long)sent);
    std::this_thread::sleep_for(milliseconds(500));
    rclcpp::shutdown();
    return 0;
}

// ROS2 (rclcpp) camera subscriber, the C++ comparison counterpart to
// cam_pub.cpp / relink_camera_sub_raw.cpp / ros2_camera_sub.py.
// Displays the live feed in an OpenCV window and prints running
// latency/FPS/bandwidth stats in the same format as the ReLink side.
//
// Run: ros2 run relink_bench_compare cam_sub [--width W] [--height H] [--compressed]

#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/image.hpp>
#include <sensor_msgs/msg/compressed_image.hpp>
#include <opencv2/opencv.hpp>
#include <chrono>
#include <cstdio>
#include <string>
#include <deque>
#include <numeric>

using namespace std::chrono;

struct Args {
    int width = 320, height = 240;
    bool compressed = false;
};

static Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--width") a.width = std::atoi(argv[++i]);
        else if (arg == "--height") a.height = std::atoi(argv[++i]);
        else if (arg == "--compressed") a.compressed = true;
    }
    return a;
}

class CameraSub : public rclcpp::Node {
public:
    explicit CameraSub(const Args& args) : Node("relink_compare_camera_sub_cpp"), args_(args) {
        rclcpp::QoS qos(rclcpp::KeepLast(1));
        qos.best_effort();

        start_ = steady_clock::now();
        if (args_.compressed) {
            sub_compressed_ = create_subscription<sensor_msgs::msg::CompressedImage>(
                "/compare/camera_compressed", qos,
                std::bind(&CameraSub::on_compressed, this, std::placeholders::_1));
        } else {
            sub_raw_ = create_subscription<sensor_msgs::msg::Image>(
                "/compare/camera_raw", qos,
                std::bind(&CameraSub::on_raw, this, std::placeholders::_1));
        }
        RCLCPP_INFO(get_logger(), "subscribed, displaying live");
    }

private:
    void record(size_t len, const rclcpp::Time& stamp) {
        double latency_ms = (now().seconds() - stamp.seconds()) * 1000.0;
        latencies_.push_back(latency_ms);
        if (latencies_.size() > 30) latencies_.pop_front();

        recv_bytes_ += len;
        ++recv_count_;
        if (recv_count_ % 30 == 0) {
            double elapsed = duration<double>(steady_clock::now() - start_).count();
            double avg_lat = std::accumulate(latencies_.begin(), latencies_.end(), 0.0) / latencies_.size();
            RCLCPP_INFO(get_logger(), "ros2: %u frames, %.1f fps, %.2f MB/s, avg latency %.2f ms, %zu bytes/frame",
                        recv_count_, recv_count_ / elapsed, recv_bytes_ / elapsed / 1e6, avg_lat, len);
        }
    }

    void on_raw(const sensor_msgs::msg::Image::SharedPtr msg) {
        cv::Mat frame(msg->height, msg->width, CV_8UC3, msg->data.data());
        cv::imshow("ROS2 camera", frame);
        cv::waitKey(1);
        record(msg->data.size(), rclcpp::Time(msg->header.stamp));
    }

    void on_compressed(const sensor_msgs::msg::CompressedImage::SharedPtr msg) {
        cv::Mat frame = cv::imdecode(msg->data, cv::IMREAD_COLOR);
        cv::imshow("ROS2 camera", frame);
        cv::waitKey(1);
        record(msg->data.size(), rclcpp::Time(msg->header.stamp));
    }

    Args args_;
    rclcpp::Subscription<sensor_msgs::msg::Image>::SharedPtr sub_raw_;
    rclcpp::Subscription<sensor_msgs::msg::CompressedImage>::SharedPtr sub_compressed_;
    steady_clock::time_point start_;
    uint32_t recv_count_ = 0;
    uint64_t recv_bytes_ = 0;
    std::deque<double> latencies_;
};

int main(int argc, char** argv) {
    rclcpp::init(argc, argv);
    Args args = parse_args(argc, argv);
    auto node = std::make_shared<CameraSub>(args);
    rclcpp::spin(node);
    rclcpp::shutdown();
    return 0;
}

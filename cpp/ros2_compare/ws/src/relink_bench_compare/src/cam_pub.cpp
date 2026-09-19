// ROS2 (rclcpp) camera publisher, the C++ comparison counterpart to
// ../../../../cpp/examples/ros2_compare/relink_camera_pub_raw.cpp and
// python/relink_py/examples/ros2_compare/ros2_camera_pub.py. Publishes
// either a raw sensor_msgs/Image (bgr8) or a JPEG-compressed
// sensor_msgs/CompressedImage (--compressed) at a configurable
// resolution and target rate.
//
// QoS is BEST_EFFORT/KEEP_LAST(1) to match ReLink's UDP,
// no-retransmission semantics as closely as ROS2 allows.
//
// Run: ros2 run relink_bench_compare cam_pub [--width W] [--height H] [--fps N] [--compressed] [--quality Q] [--video PATH]
//
// --video PATH reads frames from a video file instead of a live camera
// (looping at EOF) -- sidesteps a live webcam's exclusive-open
// semantics, useful for smoke tests / repeatable comparisons.

#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/image.hpp>
#include <sensor_msgs/msg/compressed_image.hpp>
#include <opencv2/opencv.hpp>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

using namespace std::chrono;

struct Args {
    int width = -1, height = -1;  // -1 = not explicitly set by the user
    double fps = -1.0;
    bool compressed = false;
    int quality = 70;
    std::string video;
};

static Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--width") a.width = std::atoi(argv[++i]);
        else if (arg == "--height") a.height = std::atoi(argv[++i]);
        else if (arg == "--fps") a.fps = std::atof(argv[++i]);
        else if (arg == "--compressed") a.compressed = true;
        else if (arg == "--quality") a.quality = std::atoi(argv[++i]);
        else if (arg == "--video") a.video = argv[++i];
    }
    return a;
}

class CameraPub : public rclcpp::Node {
public:
    explicit CameraPub(const Args& args) : Node("relink_compare_camera_pub_cpp"), args_(args) {
        rclcpp::QoS qos(rclcpp::KeepLast(1));
        qos.best_effort();

        if (args_.compressed) {
            pub_compressed_ = create_publisher<sensor_msgs::msg::CompressedImage>(
                "/compare/camera_compressed", qos);
        } else {
            pub_raw_ = create_publisher<sensor_msgs::msg::Image>("/compare/camera_raw", qos);
        }

        // Force V4L2 explicitly for a live camera -- OpenCV's default
        // backend probing on this system picks GStreamer, whose
        // pipeline fails outright at some resolution/fps combinations
        // this camera otherwise supports fine.
        if (args_.video.empty()) cap_.open(0, cv::CAP_V4L2);
        else cap_.open(args_.video);
        if (!cap_.isOpened()) {
            throw std::runtime_error("could not open " +
                (args_.video.empty() ? std::string("/dev/video0") : args_.video));
        }
        if (!args_.video.empty()) {
            // Follow the file's own native resolution/fps unless the
            // user explicitly overrode one.
            if (args_.width < 0) args_.width = static_cast<int>(cap_.get(cv::CAP_PROP_FRAME_WIDTH));
            if (args_.height < 0) args_.height = static_cast<int>(cap_.get(cv::CAP_PROP_FRAME_HEIGHT));
            if (args_.fps < 0) {
                double v = cap_.get(cv::CAP_PROP_FPS);
                args_.fps = v > 0 ? v : 30.0;
            }
        } else {
            if (args_.width < 0) args_.width = 320;
            if (args_.height < 0) args_.height = 240;
            if (args_.fps < 0) args_.fps = 30.0;
            cap_.set(cv::CAP_PROP_FRAME_WIDTH, args_.width);
            cap_.set(cv::CAP_PROP_FRAME_HEIGHT, args_.height);
            cap_.set(cv::CAP_PROP_FPS, args_.fps);
        }

        start_ = steady_clock::now();
        auto period = duration_cast<milliseconds>(duration<double>(1.0 / args_.fps));
        timer_ = create_wall_timer(period, std::bind(&CameraPub::on_timer, this));

        RCLCPP_INFO(get_logger(), "publishing %s frames at %dx%d, target %.1f fps, from %s",
                    args_.compressed ? "JPEG" : "raw BGR", args_.width, args_.height, args_.fps,
                    args_.video.empty() ? "camera 0" : args_.video.c_str());
    }

private:
    void on_timer() {
        cv::Mat frame;
        cap_ >> frame;
        if (frame.empty()) {
            if (!args_.video.empty()) cap_.set(cv::CAP_PROP_POS_FRAMES, 0);  // loop at EOF
            else if (++consecutive_failures_ == 30) {
                RCLCPP_WARN(get_logger(), "camera reads failing repeatedly "
                            "(device busy/still initializing?), still retrying...");
            }
            return;
        }
        consecutive_failures_ = 0;

        if (frame.cols != args_.width || frame.rows != args_.height) {
            cv::resize(frame, frame, cv::Size(args_.width, args_.height));
        }

        size_t sent_len;
        auto stamp = now();
        if (args_.compressed) {
            std::vector<uchar> jpeg;
            cv::imencode(".jpg", frame, jpeg, {cv::IMWRITE_JPEG_QUALITY, args_.quality});
            sensor_msgs::msg::CompressedImage msg;
            msg.header.stamp = stamp;
            msg.format = "jpeg";
            msg.data.assign(jpeg.begin(), jpeg.end());
            sent_len = msg.data.size();
            pub_compressed_->publish(msg);
        } else {
            sensor_msgs::msg::Image msg;
            msg.header.stamp = stamp;
            msg.height = args_.height;
            msg.width = args_.width;
            msg.encoding = "bgr8";
            msg.is_bigendian = 0;
            msg.step = args_.width * 3;
            size_t raw_len = frame.total() * frame.elemSize();
            msg.data.assign(frame.data, frame.data + raw_len);
            sent_len = msg.data.size();
            pub_raw_->publish(msg);
        }

        sent_bytes_ += sent_len;
        ++frame_id_;
        if (frame_id_ % 30 == 0) {
            double elapsed = duration<double>(steady_clock::now() - start_).count();
            RCLCPP_INFO(get_logger(), "ros2: %u frames, %.1f fps, %.2f MB/s, %zu bytes/frame",
                        frame_id_, frame_id_ / elapsed, sent_bytes_ / elapsed / 1e6, sent_len);
        }
    }

    Args args_;
    cv::VideoCapture cap_;
    rclcpp::Publisher<sensor_msgs::msg::Image>::SharedPtr pub_raw_;
    rclcpp::Publisher<sensor_msgs::msg::CompressedImage>::SharedPtr pub_compressed_;
    rclcpp::TimerBase::SharedPtr timer_;
    steady_clock::time_point start_;
    uint32_t frame_id_ = 0;
    uint64_t sent_bytes_ = 0;
    int consecutive_failures_ = 0;
};

int main(int argc, char** argv) {
    rclcpp::init(argc, argv);
    Args args = parse_args(argc, argv);
    auto node = std::make_shared<CameraPub>(args);
    rclcpp::spin(node);
    rclcpp::shutdown();
    return 0;
}

// sensor_msgs_pubsub -- publishes and subscribes every sensor_msgs
// type ReLink ships via plain advertise/publish/subscribe (relink/
// include/relink/standard_msgs.hpp): Imu, NavSatStatus, NavSatFix,
// MagneticField, Temperature, Range, RegionOfInterest, CameraInfo,
// PointField, PointCloud2, LaserScan, JointState.
//
// NOT covered here: sensor_msgs/Image and CompressedImage. Those map
// to ReLink's ImageChunk (image.hpp), a chunked large-blob type that
// goes through advertise_image/publish_image/subscribe_image instead
// -- see camera_stream.cpp and the README's "Sending images" step.
//
// Build:
//   g++ -std=c++17 -I relink/include -pthread examples/cpp/sensor_msgs_pubsub.cpp -o sensor_msgs_pubsub
// Run (in two terminals, or on two machines):
//   ./sensor_msgs_pubsub

#include "relink/relink.hpp"
#include <cstdio>
#include <chrono>
#include <thread>

using namespace geometry_msgs;
using namespace sensor_msgs;

int main() {
    RelinkNode node;
    node.use_multicast_discovery();   // zero setup -- see Step 3

    node.subscribe<Imu>("/relink/sensor/imu", [](const Imu& m) {
        std::printf("sensor_msgs/Imu       accel.z=%.2f\n", m.linear_acceleration.z);
    });
    node.subscribe<NavSatStatus>("/relink/sensor/navsat_status", [](const NavSatStatus& m) {
        std::printf("sensor_msgs/NavSatStatus status=%d\n", m.status);
    });
    node.subscribe<NavSatFix>("/relink/sensor/navsat_fix", [](const NavSatFix& m) {
        std::printf("sensor_msgs/NavSatFix lat=%.6f lon=%.6f\n", m.latitude, m.longitude);
    });
    node.subscribe<MagneticField>("/relink/sensor/mag", [](const MagneticField& m) {
        std::printf("sensor_msgs/MagneticField x=%.2f\n", m.magnetic_field.x);
    });
    node.subscribe<Temperature>("/relink/sensor/temp", [](const Temperature& m) {
        std::printf("sensor_msgs/Temperature %.1f C\n", m.temperature);
    });
    node.subscribe<Range>("/relink/sensor/range", [](const Range& m) {
        std::printf("sensor_msgs/Range     %.2f m\n", m.range);
    });
    node.subscribe<RegionOfInterest>("/relink/sensor/roi", [](const RegionOfInterest& m) {
        std::printf("sensor_msgs/RegionOfInterest %ux%u\n", m.width, m.height);
    });
    node.subscribe<CameraInfo>("/relink/sensor/camera_info", [](const CameraInfo& m) {
        std::printf("sensor_msgs/CameraInfo %ux%u model=%s\n", m.width, m.height, m.distortion_model_str().c_str());
    });
    node.subscribe<PointField>("/relink/sensor/point_field", [](const PointField& m) {
        std::printf("sensor_msgs/PointField name=%s\n", m.name_str().c_str());
    });
    node.subscribe<PointCloud2>("/relink/sensor/point_cloud2", [](const PointCloud2& m) {
        std::printf("sensor_msgs/PointCloud2 %ux%u fields=%u\n", m.width, m.height, m.fields_count);
    });
    node.subscribe<LaserScan>("/relink/sensor/laser_scan", [](const LaserScan& m) {
        std::printf("sensor_msgs/LaserScan ranges_count=%u\n", m.ranges_count);
    });
    node.subscribe<JointState>("/relink/sensor/joint_state", [](const JointState& m) {
        std::printf("sensor_msgs/JointState count=%u name0=%s\n", m.count, m.name_str(0).c_str());
    });

    node.advertise<Imu>("/relink/sensor/imu");
    node.advertise<NavSatStatus>("/relink/sensor/navsat_status");
    node.advertise<NavSatFix>("/relink/sensor/navsat_fix");
    node.advertise<MagneticField>("/relink/sensor/mag");
    node.advertise<Temperature>("/relink/sensor/temp");
    node.advertise<Range>("/relink/sensor/range");
    node.advertise<RegionOfInterest>("/relink/sensor/roi");
    node.advertise<CameraInfo>("/relink/sensor/camera_info");
    node.advertise<PointField>("/relink/sensor/point_field");
    node.advertise<PointCloud2>("/relink/sensor/point_cloud2");
    node.advertise<LaserScan>("/relink/sensor/laser_scan");
    node.advertise<JointState>("/relink/sensor/joint_state");

    int i = 0;
    while (true) {
        node.spin_once();   // services discovery -- call this every loop

        {
            Imu imu{};
            imu.header.set_frame_id("imu_link");
            imu.orientation = { 0, 0, 0, 1 };
            imu.linear_acceleration = { 0, 0, 9.81f };
            node.publish<Imu>("/relink/sensor/imu", imu);
        }
        node.publish<NavSatStatus>("/relink/sensor/navsat_status", NavSatStatus{ .status = NavSatStatus::kFix, .service = NavSatStatus::kServiceGps });
        {
            NavSatFix fix{};
            fix.header.set_frame_id("gps");
            fix.status = { NavSatStatus::kFix, NavSatStatus::kServiceGps };
            fix.latitude = 13.7563 + i * 0.0001;
            fix.longitude = 100.5018;
            node.publish<NavSatFix>("/relink/sensor/navsat_fix", fix);
        }
        {
            MagneticField mag{};
            mag.header.set_frame_id("imu_link");
            mag.magnetic_field = { 20.0f, 0.0f, 40.0f };
            node.publish<MagneticField>("/relink/sensor/mag", mag);
        }
        {
            Temperature temp{};
            temp.header.set_frame_id("cpu");
            temp.temperature = 36.6 + (i % 5);
            node.publish<Temperature>("/relink/sensor/temp", temp);
        }
        {
            Range range{};
            range.header.set_frame_id("sonar");
            range.radiation_type = Range::kUltrasound;
            range.min_range = 0.02f;
            range.max_range = 4.0f;
            range.range = 1.0f + 0.01f * (i % 100);
            node.publish<Range>("/relink/sensor/range", range);
        }
        node.publish<RegionOfInterest>("/relink/sensor/roi", RegionOfInterest{ .x_offset = 0, .y_offset = 0, .height = 480, .width = 640, .do_rectify = 1 });
        {
            CameraInfo cam{};
            cam.header.set_frame_id("camera");
            cam.width = 640;
            cam.height = 480;
            cam.set_distortion_model("plumb_bob");
            node.publish<CameraInfo>("/relink/sensor/camera_info", cam);
        }
        {
            PointField pf{};
            pf.set_name("x");
            pf.offset = 0;
            pf.datatype = PointField::kFloat32;
            pf.count = 1;
            node.publish<PointField>("/relink/sensor/point_field", pf);
        }
        {
            PointCloud2 cloud{};
            cloud.header.set_frame_id("lidar");
            cloud.width = 10;
            cloud.height = 1;
            cloud.fields_count = 1;
            cloud.fields[0].set_name("x");
            node.publish<PointCloud2>("/relink/sensor/point_cloud2", cloud);
        }
        {
            LaserScan scan{};
            scan.header.set_frame_id("lidar");
            scan.angle_min = -1.57f;
            scan.angle_max = 1.57f;
            scan.ranges_count = 3;
            scan.ranges[0] = 1.0f; scan.ranges[1] = 2.0f; scan.ranges[2] = 3.0f;
            node.publish<LaserScan>("/relink/sensor/laser_scan", scan);
        }
        {
            JointState js{};
            js.header.set_frame_id("robot");
            js.count = 2;
            js.set_name(0, "shoulder");
            js.set_name(1, "elbow");
            js.position[0] = 0.1 * i;
            js.position[1] = 0.2 * i;
            node.publish<JointState>("/relink/sensor/joint_state", js);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        ++i;
    }
}

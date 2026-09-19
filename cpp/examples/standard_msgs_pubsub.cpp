// standard_msgs_pubsub -- publishes and subscribes every ROS-familiar
// composite type ReLink ships (relink/include/relink/standard_msgs.hpp,
// what the file itself calls "NoROSLib" naming), one topic per type, in
// a single runnable file. These are built out of the primitive types
// builtin_types_pubsub.cpp covers, plus geometry_msgs/sensor_msgs/
// nav_msgs/diagnostic_msgs/trajectory_msgs/actionlib_msgs shapes ported
// from real ROS message definitions (see standard_msgs.hpp's file-level
// comment for the handful of deliberate departures: fixed-size strings,
// float32 instead of float64 for geometry_msgs, fixed-capacity arrays
// instead of unbounded ones).
//
// Types covered (49 total, everything in standard_msgs.hpp except
// sensor_msgs::ImageChunk, which needs advertise_image/publish_image/
// subscribe_image instead of plain advertise/publish/subscribe -- see
// the README's "Sending images" step and camera_stream.cpp):
//
//   std_msgs:        Empty, Time, Duration, ColorRGBA, Header, String
//   geometry_msgs:   Vector3, Point, Point32, Quaternion, Pose, Twist,
//                     Accel, Wrench, PoseStamped, TwistStamped,
//                     Transform, TransformStamped, PoseWithCovariance,
//                     TwistWithCovariance, PoseArray, Polygon
//   sensor_msgs:     Imu, NavSatStatus, NavSatFix, MagneticField,
//                     Temperature, Range, RegionOfInterest, CameraInfo,
//                     PointField, PointCloud2, LaserScan, JointState
//   nav_msgs:        Odometry, MapMetaData, Path, OccupancyGrid, GridCells
//   diagnostic_msgs: KeyValue, DiagnosticStatus, DiagnosticArray
//   trajectory_msgs: JointTrajectoryPoint, JointTrajectory,
//                     MultiDOFJointTrajectoryPoint, MultiDOFJointTrajectory
//   actionlib_msgs:  GoalID, GoalStatus, GoalStatusArray
//
// Build:
//   g++ -std=c++17 -I relink/include -pthread examples/cpp/standard_msgs_pubsub.cpp -o standard_msgs_pubsub
// Run (in two terminals, or on two machines):
//   ./standard_msgs_pubsub

#include "relink/relink.hpp"
#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>

using namespace std_msgs;
using namespace geometry_msgs;
using namespace sensor_msgs;
namespace nav = nav_msgs;
namespace diag = diagnostic_msgs;
namespace traj = trajectory_msgs;
namespace action = actionlib_msgs;

int main() {
    RelinkNode node;
    node.use_multicast_discovery();   // zero setup -- see Step 3

    // ---------------------------------------------------------------
    // Subscribe to every topic first, so an early message from a peer
    // that started first is never missed.
    // ---------------------------------------------------------------

    // --- std_msgs ---
    node.subscribe<Empty>("/relink/std/empty", [](const Empty&) {
        std::printf("std_msgs/Empty received\n");
    });
    node.subscribe<Time>("/relink/std/time", [](const Time& m) {
        std::printf("std_msgs/Time     sec=%u nsec=%u\n", m.sec, m.nsec);
    });
    node.subscribe<Duration>("/relink/std/duration", [](const Duration& m) {
        std::printf("std_msgs/Duration sec=%d nsec=%d\n", m.sec, m.nsec);
    });
    node.subscribe<ColorRGBA>("/relink/std/color", [](const ColorRGBA& m) {
        std::printf("std_msgs/ColorRGBA r=%.2f g=%.2f b=%.2f a=%.2f\n", m.r, m.g, m.b, m.a);
    });
    node.subscribe<Header>("/relink/std/header", [](const Header& m) {
        std::printf("std_msgs/Header   seq=%u frame=%s\n", m.seq, m.frame_id_str().c_str());
    });
    node.subscribe<String>("/relink/std/string", [](const String& m) {
        std::printf("std_msgs/String   \"%s\"\n", m.str().c_str());
    });

    // --- geometry_msgs ---
    node.subscribe<Vector3>("/relink/geo/vector3", [](const Vector3& m) {
        std::printf("geometry_msgs/Vector3 (%.2f, %.2f, %.2f)\n", m.x, m.y, m.z);
    });
    node.subscribe<Point>("/relink/geo/point", [](const Point& m) {
        std::printf("geometry_msgs/Point   (%.2f, %.2f, %.2f)\n", m.x, m.y, m.z);
    });
    node.subscribe<Point32>("/relink/geo/point32", [](const Point32& m) {
        std::printf("geometry_msgs/Point32 (%.2f, %.2f, %.2f)\n", m.x, m.y, m.z);
    });
    node.subscribe<Quaternion>("/relink/geo/quaternion", [](const Quaternion& m) {
        std::printf("geometry_msgs/Quaternion (%.2f, %.2f, %.2f, %.2f)\n", m.x, m.y, m.z, m.w);
    });
    node.subscribe<Pose>("/relink/geo/pose", [](const Pose& m) {
        std::printf("geometry_msgs/Pose    pos=(%.2f, %.2f, %.2f)\n", m.position.x, m.position.y, m.position.z);
    });
    node.subscribe<Twist>("/relink/geo/twist", [](const Twist& m) {
        std::printf("geometry_msgs/Twist   linear.x=%.2f angular.z=%.2f\n", m.linear.x, m.angular.z);
    });
    node.subscribe<Accel>("/relink/geo/accel", [](const Accel& m) {
        std::printf("geometry_msgs/Accel   linear.x=%.2f\n", m.linear.x);
    });
    node.subscribe<Wrench>("/relink/geo/wrench", [](const Wrench& m) {
        std::printf("geometry_msgs/Wrench  force.z=%.2f\n", m.force.z);
    });
    node.subscribe<PoseStamped>("/relink/geo/pose_stamped", [](const PoseStamped& m) {
        std::printf("geometry_msgs/PoseStamped frame=%s\n", m.header.frame_id_str().c_str());
    });
    node.subscribe<TwistStamped>("/relink/geo/twist_stamped", [](const TwistStamped& m) {
        std::printf("geometry_msgs/TwistStamped frame=%s\n", m.header.frame_id_str().c_str());
    });
    node.subscribe<Transform>("/relink/geo/transform", [](const Transform& m) {
        std::printf("geometry_msgs/Transform translation.x=%.2f\n", m.translation.x);
    });
    node.subscribe<TransformStamped>("/relink/geo/transform_stamped", [](const TransformStamped& m) {
        std::printf("geometry_msgs/TransformStamped child=%s\n", m.child_frame_id_str().c_str());
    });
    node.subscribe<PoseWithCovariance>("/relink/geo/pose_cov", [](const PoseWithCovariance& m) {
        std::printf("geometry_msgs/PoseWithCovariance cov[0]=%.2f\n", m.covariance[0]);
    });
    node.subscribe<TwistWithCovariance>("/relink/geo/twist_cov", [](const TwistWithCovariance& m) {
        std::printf("geometry_msgs/TwistWithCovariance cov[0]=%.2f\n", m.covariance[0]);
    });
    node.subscribe<PoseArray>("/relink/geo/pose_array", [](const PoseArray& m) {
        std::printf("geometry_msgs/PoseArray count=%u\n", m.count);
    });
    node.subscribe<Polygon>("/relink/geo/polygon", [](const Polygon& m) {
        std::printf("geometry_msgs/Polygon count=%u\n", m.count);
    });

    // --- sensor_msgs ---
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

    // --- nav_msgs ---
    node.subscribe<nav::Odometry>("/relink/nav/odometry", [](const nav::Odometry& m) {
        std::printf("nav_msgs/Odometry child=%s\n", m.child_frame_id_str().c_str());
    });
    node.subscribe<nav::MapMetaData>("/relink/nav/map_meta", [](const nav::MapMetaData& m) {
        std::printf("nav_msgs/MapMetaData %ux%u res=%.2f\n", m.width, m.height, m.resolution);
    });
    node.subscribe<nav::Path>("/relink/nav/path", [](const nav::Path& m) {
        std::printf("nav_msgs/Path count=%u\n", m.count);
    });
    node.subscribe<nav::OccupancyGrid>("/relink/nav/occupancy_grid", [](const nav::OccupancyGrid& m) {
        std::printf("nav_msgs/OccupancyGrid data_len=%u\n", m.data_len);
    });
    node.subscribe<nav::GridCells>("/relink/nav/grid_cells", [](const nav::GridCells& m) {
        std::printf("nav_msgs/GridCells count=%u\n", m.count);
    });

    // --- diagnostic_msgs ---
    node.subscribe<diag::KeyValue>("/relink/diag/key_value", [](const diag::KeyValue& m) {
        std::printf("diagnostic_msgs/KeyValue %s=%s\n", m.key_str().c_str(), m.value_str().c_str());
    });
    node.subscribe<diag::DiagnosticStatus>("/relink/diag/status", [](const diag::DiagnosticStatus& m) {
        std::printf("diagnostic_msgs/DiagnosticStatus level=%d name=%s\n", m.level, m.name);
    });
    node.subscribe<diag::DiagnosticArray>("/relink/diag/array", [](const diag::DiagnosticArray& m) {
        std::printf("diagnostic_msgs/DiagnosticArray status_count=%u\n", m.status_count);
    });

    // --- trajectory_msgs ---
    node.subscribe<traj::JointTrajectoryPoint>("/relink/traj/point", [](const traj::JointTrajectoryPoint& m) {
        std::printf("trajectory_msgs/JointTrajectoryPoint count=%u pos0=%.2f\n", m.count, m.positions[0]);
    });
    node.subscribe<traj::JointTrajectory>("/relink/traj/trajectory", [](const traj::JointTrajectory& m) {
        std::printf("trajectory_msgs/JointTrajectory points_count=%u\n", m.points_count);
    });
    node.subscribe<traj::MultiDOFJointTrajectoryPoint>("/relink/traj/multidof_point", [](const traj::MultiDOFJointTrajectoryPoint& m) {
        std::printf("trajectory_msgs/MultiDOFJointTrajectoryPoint count=%u\n", m.count);
    });
    node.subscribe<traj::MultiDOFJointTrajectory>("/relink/traj/multidof_trajectory", [](const traj::MultiDOFJointTrajectory& m) {
        std::printf("trajectory_msgs/MultiDOFJointTrajectory points_count=%u\n", m.points_count);
    });

    // --- actionlib_msgs ---
    node.subscribe<action::GoalID>("/relink/action/goal_id", [](const action::GoalID& m) {
        std::printf("actionlib_msgs/GoalID id=%s\n", m.id_str().c_str());
    });
    node.subscribe<action::GoalStatus>("/relink/action/goal_status", [](const action::GoalStatus& m) {
        std::printf("actionlib_msgs/GoalStatus status=%u text=%s\n", m.status, m.text_str().c_str());
    });
    node.subscribe<action::GoalStatusArray>("/relink/action/goal_status_array", [](const action::GoalStatusArray& m) {
        std::printf("actionlib_msgs/GoalStatusArray status_list_count=%u\n", m.status_list_count);
    });

    // ---------------------------------------------------------------
    // Advertise every topic (same list, same order).
    // ---------------------------------------------------------------
    node.advertise<Empty>("/relink/std/empty");
    node.advertise<Time>("/relink/std/time");
    node.advertise<Duration>("/relink/std/duration");
    node.advertise<ColorRGBA>("/relink/std/color");
    node.advertise<Header>("/relink/std/header");
    node.advertise<String>("/relink/std/string");

    node.advertise<Vector3>("/relink/geo/vector3");
    node.advertise<Point>("/relink/geo/point");
    node.advertise<Point32>("/relink/geo/point32");
    node.advertise<Quaternion>("/relink/geo/quaternion");
    node.advertise<Pose>("/relink/geo/pose");
    node.advertise<Twist>("/relink/geo/twist");
    node.advertise<Accel>("/relink/geo/accel");
    node.advertise<Wrench>("/relink/geo/wrench");
    node.advertise<PoseStamped>("/relink/geo/pose_stamped");
    node.advertise<TwistStamped>("/relink/geo/twist_stamped");
    node.advertise<Transform>("/relink/geo/transform");
    node.advertise<TransformStamped>("/relink/geo/transform_stamped");
    node.advertise<PoseWithCovariance>("/relink/geo/pose_cov");
    node.advertise<TwistWithCovariance>("/relink/geo/twist_cov");
    node.advertise<PoseArray>("/relink/geo/pose_array");
    node.advertise<Polygon>("/relink/geo/polygon");

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

    node.advertise<nav::Odometry>("/relink/nav/odometry");
    node.advertise<nav::MapMetaData>("/relink/nav/map_meta");
    node.advertise<nav::Path>("/relink/nav/path");
    node.advertise<nav::OccupancyGrid>("/relink/nav/occupancy_grid");
    node.advertise<nav::GridCells>("/relink/nav/grid_cells");

    node.advertise<diag::KeyValue>("/relink/diag/key_value");
    node.advertise<diag::DiagnosticStatus>("/relink/diag/status");
    node.advertise<diag::DiagnosticArray>("/relink/diag/array");

    node.advertise<traj::JointTrajectoryPoint>("/relink/traj/point");
    node.advertise<traj::JointTrajectory>("/relink/traj/trajectory");
    node.advertise<traj::MultiDOFJointTrajectoryPoint>("/relink/traj/multidof_point");
    node.advertise<traj::MultiDOFJointTrajectory>("/relink/traj/multidof_trajectory");

    node.advertise<action::GoalID>("/relink/action/goal_id");
    node.advertise<action::GoalStatus>("/relink/action/goal_status");
    node.advertise<action::GoalStatusArray>("/relink/action/goal_status_array");

    int i = 0;
    while (true) {
        node.spin_once();   // services discovery -- call this every loop
        float t = static_cast<float>(i) * 0.1f;

        // --- std_msgs ---
        node.publish<Empty>("/relink/std/empty", Empty{});
        node.publish<Time>("/relink/std/time", Time::now());
        node.publish<Duration>("/relink/std/duration", Duration{ .sec = i, .nsec = 0 });
        node.publish<ColorRGBA>("/relink/std/color", ColorRGBA{ .r = 1.0f, .g = 0.5f, .b = 0.0f, .a = 1.0f });
        {
            Header h{};
            h.seq = i;
            h.stamp_now();
            h.set_frame_id("base_link");
            node.publish<Header>("/relink/std/header", h);
        }
        {
            String s{};
            s.set("hello from standard_msgs_pubsub");
            node.publish<String>("/relink/std/string", s);
        }

        // --- geometry_msgs ---
        node.publish<Vector3>("/relink/geo/vector3", Vector3{ .x = t, .y = 0, .z = 0 });
        node.publish<Point>("/relink/geo/point", Point{ .x = t, .y = 0, .z = 0 });
        node.publish<Point32>("/relink/geo/point32", Point32{ .x = t, .y = 0, .z = 0 });
        node.publish<Quaternion>("/relink/geo/quaternion", Quaternion{ .x = 0, .y = 0, .z = 0, .w = 1 });
        node.publish<Pose>("/relink/geo/pose", Pose{ .position = { t, 0, 0 }, .orientation = { 0, 0, 0, 1 } });
        node.publish<Twist>("/relink/geo/twist", Twist{ .linear = { t, 0, 0 }, .angular = { 0, 0, 0.1f } });
        node.publish<Accel>("/relink/geo/accel", Accel{ .linear = { t, 0, 0 }, .angular = { 0, 0, 0 } });
        node.publish<Wrench>("/relink/geo/wrench", Wrench{ .force = { 0, 0, t }, .torque = { 0, 0, 0 } });
        {
            PoseStamped ps{};
            ps.header.set_frame_id("map");
            ps.pose.position = { t, 0, 0 };
            node.publish<PoseStamped>("/relink/geo/pose_stamped", ps);
        }
        {
            TwistStamped ts{};
            ts.header.set_frame_id("map");
            node.publish<TwistStamped>("/relink/geo/twist_stamped", ts);
        }
        node.publish<Transform>("/relink/geo/transform", Transform{ .translation = { t, 0, 0 }, .rotation = { 0, 0, 0, 1 } });
        {
            TransformStamped tfs{};
            tfs.set_child_frame_id("base_link");
            node.publish<TransformStamped>("/relink/geo/transform_stamped", tfs);
        }
        {
            PoseWithCovariance pc{};
            pc.pose.position = { t, 0, 0 };
            node.publish<PoseWithCovariance>("/relink/geo/pose_cov", pc);
        }
        {
            TwistWithCovariance tc{};
            tc.twist.linear = { t, 0, 0 };
            node.publish<TwistWithCovariance>("/relink/geo/twist_cov", tc);
        }
        {
            PoseArray pa{};
            pa.count = 2;
            pa.poses[0].position = { 0, 0, 0 };
            pa.poses[1].position = { t, 0, 0 };
            node.publish<PoseArray>("/relink/geo/pose_array", pa);
        }
        {
            Polygon poly{};
            poly.count = 3;
            poly.points[0] = { 0, 0, 0 };
            poly.points[1] = { 1, 0, 0 };
            poly.points[2] = { 0, 1, 0 };
            node.publish<Polygon>("/relink/geo/polygon", poly);
        }

        // --- sensor_msgs ---
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

        // --- nav_msgs ---
        {
            nav::Odometry odom{};
            odom.header.set_frame_id("odom");
            odom.set_child_frame_id("base_link");
            odom.pose.position = { t, 0, 0 };
            node.publish<nav::Odometry>("/relink/nav/odometry", odom);
        }
        {
            nav::MapMetaData meta{};
            meta.map_load_time = Time::now();
            meta.resolution = 0.05f;
            meta.width = 100;
            meta.height = 100;
            node.publish<nav::MapMetaData>("/relink/nav/map_meta", meta);
        }
        {
            nav::Path path{};
            path.header.set_frame_id("map");
            path.count = 2;
            path.poses[0].pose.position = { 0, 0, 0 };
            path.poses[1].pose.position = { t, 0, 0 };
            node.publish<nav::Path>("/relink/nav/path", path);
        }
        {
            nav::OccupancyGrid grid{};
            grid.header.set_frame_id("map");
            grid.info.width = 10;
            grid.info.height = 10;
            grid.data_len = 100;
            node.publish<nav::OccupancyGrid>("/relink/nav/occupancy_grid", grid);
        }
        {
            nav::GridCells cells{};
            cells.header.set_frame_id("map");
            cells.cell_width = 0.1f;
            cells.cell_height = 0.1f;
            cells.count = 2;
            cells.cells[0] = { 0, 0, 0 };
            cells.cells[1] = { t, 0, 0 };
            node.publish<nav::GridCells>("/relink/nav/grid_cells", cells);
        }

        // --- diagnostic_msgs ---
        {
            diag::KeyValue kv{};
            kv.set_key("battery_pct");
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%d", 100 - (i % 100));
            kv.set_value(buf);
            node.publish<diag::KeyValue>("/relink/diag/key_value", kv);
        }
        {
            diag::DiagnosticStatus status{};
            status.level = diag::DiagnosticStatus::kOk;
            status.set_name("battery_monitor");
            status.set_message("nominal");
            status.set_hardware_id("bms_01");
            node.publish<diag::DiagnosticStatus>("/relink/diag/status", status);
        }
        {
            diag::DiagnosticArray arr{};
            arr.header.set_frame_id("diagnostics");
            arr.status_count = 1;
            arr.status[0].level = diag::DiagnosticStatus::kOk;
            arr.status[0].set_name("battery_monitor");
            node.publish<diag::DiagnosticArray>("/relink/diag/array", arr);
        }

        // --- trajectory_msgs ---
        {
            traj::JointTrajectoryPoint pt{};
            pt.count = 2;
            pt.positions[0] = 0.1 * i;
            pt.positions[1] = 0.2 * i;
            node.publish<traj::JointTrajectoryPoint>("/relink/traj/point", pt);
        }
        {
            traj::JointTrajectory tj{};
            tj.header.set_frame_id("robot");
            tj.joint_names_count = 2;
            tj.set_joint_name(0, "shoulder");
            tj.set_joint_name(1, "elbow");
            tj.points_count = 1;
            tj.points[0].count = 2;
            node.publish<traj::JointTrajectory>("/relink/traj/trajectory", tj);
        }
        {
            traj::MultiDOFJointTrajectoryPoint mdp{};
            mdp.count = 1;
            mdp.transforms[0].translation = { t, 0, 0 };
            node.publish<traj::MultiDOFJointTrajectoryPoint>("/relink/traj/multidof_point", mdp);
        }
        {
            traj::MultiDOFJointTrajectory mdt{};
            mdt.header.set_frame_id("robot");
            mdt.joint_names_count = 1;
            mdt.set_joint_name(0, "base");
            mdt.points_count = 1;
            node.publish<traj::MultiDOFJointTrajectory>("/relink/traj/multidof_trajectory", mdt);
        }

        // --- actionlib_msgs ---
        {
            action::GoalID gid{};
            gid.stamp = Time::now();
            char buf[32];
            std::snprintf(buf, sizeof(buf), "goal_%d", i);
            gid.set_id(buf);
            node.publish<action::GoalID>("/relink/action/goal_id", gid);
        }
        {
            action::GoalStatus gs{};
            gs.status = action::GoalStatus::kActive;
            gs.set_text("moving to goal");
            node.publish<action::GoalStatus>("/relink/action/goal_status", gs);
        }
        {
            action::GoalStatusArray gsa{};
            gsa.header.set_frame_id("action_server");
            gsa.status_list_count = 1;
            gsa.status_list[0].status = action::GoalStatus::kActive;
            node.publish<action::GoalStatusArray>("/relink/action/goal_status_array", gsa);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        ++i;
    }
}

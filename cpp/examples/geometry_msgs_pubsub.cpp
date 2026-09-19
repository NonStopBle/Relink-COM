// geometry_msgs_pubsub -- publishes and subscribes every geometry_msgs
// type ReLink ships (relink/include/relink/standard_msgs.hpp), one
// topic per type: Vector3, Point, Point32, Quaternion, Pose, Twist,
// Accel, Wrench, PoseStamped, TwistStamped, Transform,
// TransformStamped, PoseWithCovariance, TwistWithCovariance,
// PoseArray, Polygon.
//
// Build:
//   g++ -std=c++17 -I relink/include -pthread examples/cpp/geometry_msgs_pubsub.cpp -o geometry_msgs_pubsub
// Run (in two terminals, or on two machines):
//   ./geometry_msgs_pubsub

#include "relink/relink.hpp"
#include <cstdio>
#include <chrono>
#include <thread>

using namespace geometry_msgs;

int main() {
    RelinkNode node;
    node.use_multicast_discovery();   // zero setup -- see Step 3

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

    int i = 0;
    while (true) {
        node.spin_once();   // services discovery -- call this every loop
        float t = static_cast<float>(i) * 0.1f;

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

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        ++i;
    }
}

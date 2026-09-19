// trajectory_msgs_pubsub -- publishes and subscribes every
// trajectory_msgs type ReLink ships (relink/include/relink/
// standard_msgs.hpp), one topic per type: JointTrajectoryPoint,
// JointTrajectory, MultiDOFJointTrajectoryPoint, MultiDOFJointTrajectory.
//
// Build:
//   g++ -std=c++17 -I relink/include -pthread examples/cpp/trajectory_msgs_pubsub.cpp -o trajectory_msgs_pubsub
// Run (in two terminals, or on two machines):
//   ./trajectory_msgs_pubsub

#include "relink/relink.hpp"
#include <cstdio>
#include <chrono>
#include <thread>

using namespace geometry_msgs;
using namespace trajectory_msgs;

int main() {
    RelinkNode node;
    node.use_multicast_discovery();   // zero setup -- see Step 3

    node.subscribe<JointTrajectoryPoint>("/relink/traj/point", [](const JointTrajectoryPoint& m) {
        std::printf("trajectory_msgs/JointTrajectoryPoint count=%u pos0=%.2f\n", m.count, m.positions[0]);
    });
    node.subscribe<JointTrajectory>("/relink/traj/trajectory", [](const JointTrajectory& m) {
        std::printf("trajectory_msgs/JointTrajectory points_count=%u\n", m.points_count);
    });
    node.subscribe<MultiDOFJointTrajectoryPoint>("/relink/traj/multidof_point", [](const MultiDOFJointTrajectoryPoint& m) {
        std::printf("trajectory_msgs/MultiDOFJointTrajectoryPoint count=%u\n", m.count);
    });
    node.subscribe<MultiDOFJointTrajectory>("/relink/traj/multidof_trajectory", [](const MultiDOFJointTrajectory& m) {
        std::printf("trajectory_msgs/MultiDOFJointTrajectory points_count=%u\n", m.points_count);
    });

    node.advertise<JointTrajectoryPoint>("/relink/traj/point");
    node.advertise<JointTrajectory>("/relink/traj/trajectory");
    node.advertise<MultiDOFJointTrajectoryPoint>("/relink/traj/multidof_point");
    node.advertise<MultiDOFJointTrajectory>("/relink/traj/multidof_trajectory");

    int i = 0;
    while (true) {
        node.spin_once();   // services discovery -- call this every loop
        float t = static_cast<float>(i) * 0.1f;

        {
            JointTrajectoryPoint pt{};
            pt.count = 2;
            pt.positions[0] = 0.1 * i;
            pt.positions[1] = 0.2 * i;
            node.publish<JointTrajectoryPoint>("/relink/traj/point", pt);
        }
        {
            JointTrajectory tj{};
            tj.header.set_frame_id("robot");
            tj.joint_names_count = 2;
            tj.set_joint_name(0, "shoulder");
            tj.set_joint_name(1, "elbow");
            tj.points_count = 1;
            tj.points[0].count = 2;
            node.publish<JointTrajectory>("/relink/traj/trajectory", tj);
        }
        {
            MultiDOFJointTrajectoryPoint mdp{};
            mdp.count = 1;
            mdp.transforms[0].translation = { t, 0, 0 };
            node.publish<MultiDOFJointTrajectoryPoint>("/relink/traj/multidof_point", mdp);
        }
        {
            MultiDOFJointTrajectory mdt{};
            mdt.header.set_frame_id("robot");
            mdt.joint_names_count = 1;
            mdt.set_joint_name(0, "base");
            mdt.points_count = 1;
            node.publish<MultiDOFJointTrajectory>("/relink/traj/multidof_trajectory", mdt);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        ++i;
    }
}

#!/usr/bin/env python3
"""trajectory_msgs_pubsub -- publishes and subscribes every
trajectory_msgs type ReLink ships (relink/standard_msgs.py), one topic
per type: JointTrajectoryPoint, JointTrajectory,
MultiDOFJointTrajectoryPoint, MultiDOFJointTrajectory. Wire-compatible
with cpp/examples/trajectory_msgs_pubsub.cpp.

Run (in two terminals, or on two machines):
    python3 trajectory_msgs_pubsub.py
"""
import sys
import os
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from relink import RelinkNode
from relink import geometry_msgs, trajectory_msgs


def main():
    node = RelinkNode()
    node.use_multicast_discovery()   # zero setup -- see Step 3

    node.subscribe("/relink/traj/point", trajectory_msgs.JointTrajectoryPoint, lambda m: print(f"trajectory_msgs/JointTrajectoryPoint count={m.count} pos0={m.positions[0]:.2f}"))
    node.subscribe("/relink/traj/trajectory", trajectory_msgs.JointTrajectory, lambda m: print(f"trajectory_msgs/JointTrajectory points_count={m.points_count}"))
    node.subscribe("/relink/traj/multidof_point", trajectory_msgs.MultiDOFJointTrajectoryPoint, lambda m: print(f"trajectory_msgs/MultiDOFJointTrajectoryPoint count={m.count}"))
    node.subscribe("/relink/traj/multidof_trajectory", trajectory_msgs.MultiDOFJointTrajectory, lambda m: print(f"trajectory_msgs/MultiDOFJointTrajectory points_count={m.points_count}"))

    node.advertise("/relink/traj/point", trajectory_msgs.JointTrajectoryPoint)
    node.advertise("/relink/traj/trajectory", trajectory_msgs.JointTrajectory)
    node.advertise("/relink/traj/multidof_point", trajectory_msgs.MultiDOFJointTrajectoryPoint)
    node.advertise("/relink/traj/multidof_trajectory", trajectory_msgs.MultiDOFJointTrajectory)

    i = 0
    while True:
        node.spin_once()   # services discovery -- call this every loop
        t = i * 0.1

        pt = trajectory_msgs.JointTrajectoryPoint()
        pt.count = 2
        pt.positions[0] = 0.1 * i
        pt.positions[1] = 0.2 * i
        node.publish("/relink/traj/point", pt)

        tj = trajectory_msgs.JointTrajectory()
        tj.header.set_frame_id("robot")
        tj.joint_names_count = 2
        tj.set_joint_name(0, "shoulder")
        tj.set_joint_name(1, "elbow")
        tj.points_count = 1
        tj.points[0].count = 2
        node.publish("/relink/traj/trajectory", tj)

        mdp = trajectory_msgs.MultiDOFJointTrajectoryPoint()
        mdp.count = 1
        mdp.transforms[0].translation = geometry_msgs.Vector3(x=t, y=0, z=0)
        node.publish("/relink/traj/multidof_point", mdp)

        mdt = trajectory_msgs.MultiDOFJointTrajectory()
        mdt.header.set_frame_id("robot")
        mdt.joint_names_count = 1
        mdt.set_joint_name(0, "base")
        mdt.points_count = 1
        node.publish("/relink/traj/multidof_trajectory", mdt)

        time.sleep(0.5)
        i += 1


if __name__ == "__main__":
    main()

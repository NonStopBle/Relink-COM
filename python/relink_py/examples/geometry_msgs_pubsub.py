#!/usr/bin/env python3
"""geometry_msgs_pubsub -- publishes and subscribes every geometry_msgs
type ReLink ships (relink/standard_msgs.py), one topic per type:
Vector3, Point, Point32, Quaternion, Pose, Twist, Accel, Wrench,
PoseStamped, TwistStamped, Transform, TransformStamped,
PoseWithCovariance, TwistWithCovariance, PoseArray, Polygon.
Wire-compatible with cpp/examples/geometry_msgs_pubsub.cpp.

Run (in two terminals, or on two machines):
    python3 geometry_msgs_pubsub.py
"""
import sys
import os
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from relink import RelinkNode
from relink import geometry_msgs


def main():
    node = RelinkNode()
    node.use_multicast_discovery()   # zero setup -- see Step 3

    node.subscribe("/relink/geo/vector3", geometry_msgs.Vector3, lambda m: print(f"geometry_msgs/Vector3 ({m.x:.2f}, {m.y:.2f}, {m.z:.2f})"))
    node.subscribe("/relink/geo/point", geometry_msgs.Point, lambda m: print(f"geometry_msgs/Point   ({m.x:.2f}, {m.y:.2f}, {m.z:.2f})"))
    node.subscribe("/relink/geo/point32", geometry_msgs.Point32, lambda m: print(f"geometry_msgs/Point32 ({m.x:.2f}, {m.y:.2f}, {m.z:.2f})"))
    node.subscribe("/relink/geo/quaternion", geometry_msgs.Quaternion, lambda m: print(f"geometry_msgs/Quaternion ({m.x:.2f}, {m.y:.2f}, {m.z:.2f}, {m.w:.2f})"))
    node.subscribe("/relink/geo/pose", geometry_msgs.Pose, lambda m: print(f"geometry_msgs/Pose    pos=({m.position.x:.2f}, {m.position.y:.2f}, {m.position.z:.2f})"))
    node.subscribe("/relink/geo/twist", geometry_msgs.Twist, lambda m: print(f"geometry_msgs/Twist   linear.x={m.linear.x:.2f} angular.z={m.angular.z:.2f}"))
    node.subscribe("/relink/geo/accel", geometry_msgs.Accel, lambda m: print(f"geometry_msgs/Accel   linear.x={m.linear.x:.2f}"))
    node.subscribe("/relink/geo/wrench", geometry_msgs.Wrench, lambda m: print(f"geometry_msgs/Wrench  force.z={m.force.z:.2f}"))
    node.subscribe("/relink/geo/pose_stamped", geometry_msgs.PoseStamped, lambda m: print(f"geometry_msgs/PoseStamped frame={m.header.frame_id_str()}"))
    node.subscribe("/relink/geo/twist_stamped", geometry_msgs.TwistStamped, lambda m: print(f"geometry_msgs/TwistStamped frame={m.header.frame_id_str()}"))
    node.subscribe("/relink/geo/transform", geometry_msgs.Transform, lambda m: print(f"geometry_msgs/Transform translation.x={m.translation.x:.2f}"))
    node.subscribe("/relink/geo/transform_stamped", geometry_msgs.TransformStamped, lambda m: print(f"geometry_msgs/TransformStamped child={m.child_frame_id_str()}"))
    node.subscribe("/relink/geo/pose_cov", geometry_msgs.PoseWithCovariance, lambda m: print(f"geometry_msgs/PoseWithCovariance cov[0]={m.covariance[0]:.2f}"))
    node.subscribe("/relink/geo/twist_cov", geometry_msgs.TwistWithCovariance, lambda m: print(f"geometry_msgs/TwistWithCovariance cov[0]={m.covariance[0]:.2f}"))
    node.subscribe("/relink/geo/pose_array", geometry_msgs.PoseArray, lambda m: print(f"geometry_msgs/PoseArray count={m.count}"))
    node.subscribe("/relink/geo/polygon", geometry_msgs.Polygon, lambda m: print(f"geometry_msgs/Polygon count={m.count}"))

    node.advertise("/relink/geo/vector3", geometry_msgs.Vector3)
    node.advertise("/relink/geo/point", geometry_msgs.Point)
    node.advertise("/relink/geo/point32", geometry_msgs.Point32)
    node.advertise("/relink/geo/quaternion", geometry_msgs.Quaternion)
    node.advertise("/relink/geo/pose", geometry_msgs.Pose)
    node.advertise("/relink/geo/twist", geometry_msgs.Twist)
    node.advertise("/relink/geo/accel", geometry_msgs.Accel)
    node.advertise("/relink/geo/wrench", geometry_msgs.Wrench)
    node.advertise("/relink/geo/pose_stamped", geometry_msgs.PoseStamped)
    node.advertise("/relink/geo/twist_stamped", geometry_msgs.TwistStamped)
    node.advertise("/relink/geo/transform", geometry_msgs.Transform)
    node.advertise("/relink/geo/transform_stamped", geometry_msgs.TransformStamped)
    node.advertise("/relink/geo/pose_cov", geometry_msgs.PoseWithCovariance)
    node.advertise("/relink/geo/twist_cov", geometry_msgs.TwistWithCovariance)
    node.advertise("/relink/geo/pose_array", geometry_msgs.PoseArray)
    node.advertise("/relink/geo/polygon", geometry_msgs.Polygon)

    i = 0
    while True:
        node.spin_once()   # services discovery -- call this every loop
        t = i * 0.1

        node.publish("/relink/geo/vector3", geometry_msgs.Vector3(x=t, y=0, z=0))
        node.publish("/relink/geo/point", geometry_msgs.Point(x=t, y=0, z=0))
        node.publish("/relink/geo/point32", geometry_msgs.Point32(x=t, y=0, z=0))
        node.publish("/relink/geo/quaternion", geometry_msgs.Quaternion(x=0, y=0, z=0, w=1))

        pose = geometry_msgs.Pose()
        pose.position = geometry_msgs.Point(x=t, y=0, z=0)
        pose.orientation = geometry_msgs.Quaternion(x=0, y=0, z=0, w=1)
        node.publish("/relink/geo/pose", pose)

        twist = geometry_msgs.Twist()
        twist.linear = geometry_msgs.Vector3(x=t, y=0, z=0)
        twist.angular = geometry_msgs.Vector3(x=0, y=0, z=0.1)
        node.publish("/relink/geo/twist", twist)

        accel = geometry_msgs.Accel()
        accel.linear = geometry_msgs.Vector3(x=t, y=0, z=0)
        node.publish("/relink/geo/accel", accel)

        wrench = geometry_msgs.Wrench()
        wrench.force = geometry_msgs.Vector3(x=0, y=0, z=t)
        node.publish("/relink/geo/wrench", wrench)

        ps = geometry_msgs.PoseStamped()
        ps.header.set_frame_id("map")
        ps.pose.position = geometry_msgs.Point(x=t, y=0, z=0)
        node.publish("/relink/geo/pose_stamped", ps)

        ts = geometry_msgs.TwistStamped()
        ts.header.set_frame_id("map")
        node.publish("/relink/geo/twist_stamped", ts)

        transform = geometry_msgs.Transform()
        transform.translation = geometry_msgs.Vector3(x=t, y=0, z=0)
        transform.rotation = geometry_msgs.Quaternion(x=0, y=0, z=0, w=1)
        node.publish("/relink/geo/transform", transform)

        tfs = geometry_msgs.TransformStamped()
        tfs.set_child_frame_id("base_link")
        node.publish("/relink/geo/transform_stamped", tfs)

        pc = geometry_msgs.PoseWithCovariance()
        pc.pose.position = geometry_msgs.Point(x=t, y=0, z=0)
        node.publish("/relink/geo/pose_cov", pc)

        tc = geometry_msgs.TwistWithCovariance()
        tc.twist.linear = geometry_msgs.Vector3(x=t, y=0, z=0)
        node.publish("/relink/geo/twist_cov", tc)

        pa = geometry_msgs.PoseArray()
        pa.count = 2
        pa.poses[0].position = geometry_msgs.Point(x=0, y=0, z=0)
        pa.poses[1].position = geometry_msgs.Point(x=t, y=0, z=0)
        node.publish("/relink/geo/pose_array", pa)

        poly = geometry_msgs.Polygon()
        poly.count = 3
        poly.points[0] = geometry_msgs.Point32(x=0, y=0, z=0)
        poly.points[1] = geometry_msgs.Point32(x=1, y=0, z=0)
        poly.points[2] = geometry_msgs.Point32(x=0, y=1, z=0)
        node.publish("/relink/geo/polygon", poly)

        time.sleep(0.5)
        i += 1


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""ROS2 (rclpy) camera subscriber, the comparison counterpart to
relink_camera_sub_raw.py. Displays the live feed in a cv2 window and
prints the same running latency/FPS/bandwidth stats format, so its
printed stat lines can be diffed directly against ReLink's.

Latency is computed from msg.header.stamp (set by ros2_camera_pub.py at
publish time) vs receipt time -- same measurement basis as the ReLink
side's embedded send-timestamp, so the two numbers are comparable
(both pub/sub pairs should run on the same host, or clock-synced hosts).

Requires ROS2 (rclpy, sensor_msgs) sourced, and OpenCV.

Run (after `source /opt/ros/<distro>/setup.bash` or equivalent):
    python3 ros2_camera_sub.py [--width W] [--height H] [--compressed]
"""
import argparse
import sys
import time

import rclpy
from rclpy.node import Node
from rclpy.qos import QoSProfile, ReliabilityPolicy, HistoryPolicy
from sensor_msgs.msg import Image, CompressedImage

try:
    import cv2
    import numpy as np
except ImportError:
    print("ros2_camera_sub.py requires OpenCV -- pip install opencv-python", file=sys.stderr)
    sys.exit(1)


def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--width", type=int, default=320)
    p.add_argument("--height", type=int, default=240)
    p.add_argument("--compressed", action="store_true")
    return p.parse_args()


class CameraSub(Node):
    def __init__(self, args):
        super().__init__("relink_compare_camera_sub")
        self.args = args
        qos = QoSProfile(
            reliability=ReliabilityPolicy.BEST_EFFORT,
            history=HistoryPolicy.KEEP_LAST,
            depth=1,
        )
        topic = "/compare/camera_compressed" if args.compressed else "/compare/camera_raw"
        msg_type = CompressedImage if args.compressed else Image
        self.sub = self.create_subscription(msg_type, topic, self.on_frame, qos)

        self.recv_bytes = 0
        self.recv_count = 0
        self.latencies = []
        self.start = time.time()
        self.get_logger().info(f"subscribed to {topic}, displaying live")

    def on_frame(self, msg):
        stamp_sec = msg.header.stamp.sec + msg.header.stamp.nanosec / 1e9
        latency_ms = (time.time() - stamp_sec) * 1000.0
        self.latencies.append(latency_ms)

        data = bytes(msg.data)
        if self.args.compressed:
            arr = cv2.imdecode(np.frombuffer(data, dtype=np.uint8), cv2.IMREAD_COLOR)
        else:
            arr = np.frombuffer(data, dtype=np.uint8).reshape((msg.height, msg.width, 3))
        cv2.imshow("ROS2 camera", arr)
        cv2.waitKey(1)

        self.recv_bytes += len(data)
        self.recv_count += 1
        if self.recv_count % 30 == 0:
            elapsed = time.time() - self.start
            avg_lat = sum(self.latencies[-30:]) / len(self.latencies[-30:])
            self.get_logger().info(
                f"ros2: {self.recv_count} frames, {self.recv_count/elapsed:.1f} fps, "
                f"{self.recv_bytes/elapsed/1e6:.2f} MB/s, avg latency {avg_lat:.2f} ms, "
                f"{len(data)} bytes/frame")


def main():
    args = parse_args()
    rclpy.init()
    node = CameraSub(args)
    try:
        rclpy.spin(node)
    except (KeyboardInterrupt, rclpy.executors.ExternalShutdownException):
        pass
    finally:
        node.destroy_node()
        if rclpy.ok():
            rclpy.shutdown()


if __name__ == "__main__":
    main()

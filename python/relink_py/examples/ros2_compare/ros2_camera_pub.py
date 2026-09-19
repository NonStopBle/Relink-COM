#!/usr/bin/env python3
"""ROS2 (rclpy) camera publisher, the comparison counterpart to
relink_camera_pub_raw.py. Publishes either a raw sensor_msgs/Image
(bgr8) or a JPEG-compressed sensor_msgs/CompressedImage (--compressed)
at a configurable resolution and target rate -- no colcon package
needed, this is a plain rclpy script.

QoS is set to BEST_EFFORT/KEEP_LAST(1) to match ReLink's UDP,
no-retransmission, latest-wins semantics as closely as ROS2/DDS allows,
so the comparison isn't skewed by ROS2's default RELIABLE QoS doing
retransmission work ReLink never does.

Requires ROS2 (rclpy, sensor_msgs) sourced, and OpenCV.

Run (after `source /opt/ros/<distro>/setup.bash` or equivalent):
    python3 ros2_camera_pub.py [--width W] [--height H] [--fps N]
                                [--compressed] [--quality Q] [--video PATH]

--video PATH reads frames from a video file instead of a live camera
(looping at EOF) -- see relink_camera_pub_raw.py's docstring for why.
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
except ImportError:
    print("ros2_camera_pub.py requires OpenCV -- pip install opencv-python", file=sys.stderr)
    sys.exit(1)


def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--width", type=int, default=None)
    p.add_argument("--height", type=int, default=None)
    p.add_argument("--fps", type=float, default=None)
    p.add_argument("--compressed", action="store_true")
    p.add_argument("--quality", type=int, default=70)
    p.add_argument("--video", default=None)
    return p.parse_args()


class CameraPub(Node):
    def __init__(self, args):
        super().__init__("relink_compare_camera_pub")
        self.args = args
        qos = QoSProfile(
            reliability=ReliabilityPolicy.BEST_EFFORT,
            history=HistoryPolicy.KEEP_LAST,
            depth=1,
        )
        topic = "/compare/camera_compressed" if args.compressed else "/compare/camera_raw"
        msg_type = CompressedImage if args.compressed else Image
        self.pub = self.create_publisher(msg_type, topic, qos)

        source = args.video if args.video else 0
        self.cap = cv2.VideoCapture(source, cv2.CAP_V4L2) if not args.video else cv2.VideoCapture(source)
        if not self.cap.isOpened():
            raise RuntimeError(f"could not open {source!r}")

        if args.video:
            # Follow the file's own native resolution/fps unless the
            # user explicitly overrode one.
            if args.width is None:
                args.width = int(self.cap.get(cv2.CAP_PROP_FRAME_WIDTH))
            if args.height is None:
                args.height = int(self.cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
            if args.fps is None:
                args.fps = self.cap.get(cv2.CAP_PROP_FPS) or 30.0
        else:
            if args.width is None:
                args.width = 320
            if args.height is None:
                args.height = 240
            if args.fps is None:
                args.fps = 30.0
            self.cap.set(cv2.CAP_PROP_FRAME_WIDTH, args.width)
            self.cap.set(cv2.CAP_PROP_FRAME_HEIGHT, args.height)
            self.cap.set(cv2.CAP_PROP_FPS, args.fps)

        self.frame_id = 0
        self.sent_bytes = 0
        self.consecutive_failures = 0
        self.start = time.time()
        self.timer = self.create_timer(1.0 / args.fps, self.on_timer)
        mode = f"JPEG q={args.quality}" if args.compressed else "raw BGR"
        src_desc = f"video {args.video}" if args.video else "camera 0"
        self.get_logger().info(
            f"publishing {mode} frames at {args.width}x{args.height}, "
            f"target {args.fps} fps, from {src_desc}, on {topic}")

    def on_timer(self):
        ok, frame = self.cap.read()
        if not ok:
            if self.args.video:
                self.cap.set(cv2.CAP_PROP_POS_FRAMES, 0)
                return
            self.consecutive_failures += 1
            if self.consecutive_failures == 30:
                self.get_logger().warn("camera reads failing repeatedly "
                                        "(device busy/still initializing?), still retrying...")
            return
        self.consecutive_failures = 0

        if self.args.width and (frame.shape[1], frame.shape[0]) != (self.args.width, self.args.height):
            frame = cv2.resize(frame, (self.args.width, self.args.height))

        now = self.get_clock().now().to_msg()
        if self.args.compressed:
            ok, jpeg = cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, self.args.quality])
            msg = CompressedImage()
            msg.header.stamp = now
            msg.format = "jpeg"
            msg.data = jpeg.tobytes()
        else:
            msg = Image()
            msg.header.stamp = now
            msg.height = self.args.height
            msg.width = self.args.width
            msg.encoding = "bgr8"
            msg.is_bigendian = 0
            msg.step = self.args.width * 3
            msg.data = frame.tobytes()

        self.pub.publish(msg)
        self.sent_bytes += len(msg.data)
        self.frame_id += 1

        if self.frame_id % 30 == 0:
            elapsed = time.time() - self.start
            self.get_logger().info(
                f"ros2: {self.frame_id} frames, {self.frame_id/elapsed:.1f} fps, "
                f"{self.sent_bytes/elapsed/1e6:.2f} MB/s, {len(msg.data)} bytes/frame")


def main():
    args = parse_args()
    rclpy.init()
    node = CameraPub(args)
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

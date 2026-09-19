#!/usr/bin/env python3
"""ReLink camera publisher, used for a head-to-head comparison against
ROS2's rclpy Image publisher (see ros2_camera_pub.py in this same
directory). Publishes either raw BGR bytes or JPEG-compressed bytes
(--compressed) at a configurable resolution and target rate, so the two
systems can be compared moving exactly the same bytes per frame under
whatever settings you choose.

An 8-byte send-timestamp (double, seconds, struct 'd') is prepended to
each frame's payload so the subscriber can measure end-to-end latency
without relying on clock sync tricks beyond both processes sharing the
same host clock (run pub/sub on the same machine, or on clock-synced
machines, for the latency numbers to mean anything).

Requires OpenCV: pip install opencv-python

Run:
    python3 relink_camera_pub_raw.py [rlcore_ip] [--width W] [--height H]
                                      [--fps N] [--compressed] [--quality Q]
                                      [--video PATH]

Defaults: 320x240, 30 fps target, raw (uncompressed), live camera 0.
--quality only applies with --compressed (JPEG quality 0-100, default
70). --video PATH reads frames from a video file instead of a live
camera (looping at EOF) -- useful for smoke tests / repeatable
comparisons, and sidesteps a live webcam's exclusive-open semantics
(only one process can hold a real /dev/video0 at a time). With --video
and no explicit --width/--height/--fps, the file's own native
resolution and frame rate are used (no forced resize/pacing mismatch)
-- pass --width/--height/--fps explicitly to override.
"""
import sys
import os
import argparse
import struct
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
from relink import RelinkNode

try:
    import cv2
except ImportError:
    print("relink_camera_pub_raw.py requires OpenCV -- pip install opencv-python", file=sys.stderr)
    sys.exit(1)

TOPIC_CAMERA = "/compare/camera"


def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("rlcore_ip", nargs="?", default=None)
    p.add_argument("--width", type=int, default=None)
    p.add_argument("--height", type=int, default=None)
    p.add_argument("--fps", type=float, default=None)
    p.add_argument("--compressed", action="store_true")
    p.add_argument("--quality", type=int, default=70)
    p.add_argument("--video", default=None)
    return p.parse_args()


def main():
    args = parse_args()

    node = RelinkNode()
    if args.rlcore_ip:
        node.set_rlcore.ip(args.rlcore_ip)
    else:
        node.use_multicast_discovery()

    node.advertise_image(TOPIC_CAMERA)
    node.spin_once()

    source = args.video if args.video else 0
    # Force V4L2 explicitly for a live camera -- OpenCV's default backend
    # probing on some systems picks GStreamer, whose pipeline can fail
    # outright at resolution/fps combinations V4L2 handles fine; a video
    # file just uses OpenCV's normal file-backend autodetection.
    cap = cv2.VideoCapture(source, cv2.CAP_V4L2) if not args.video else cv2.VideoCapture(source)
    if not cap.isOpened():
        print(f"relink_camera_pub_raw: could not open {source!r}", file=sys.stderr)
        return 1

    if args.video:
        # Follow the file's own native resolution/fps unless the user
        # explicitly overrode one -- no surprise resize or pacing
        # mismatch against the source material.
        if args.width is None:
            args.width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
        if args.height is None:
            args.height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
        if args.fps is None:
            args.fps = cap.get(cv2.CAP_PROP_FPS) or 30.0
    else:
        if args.width is None:
            args.width = 320
        if args.height is None:
            args.height = 240
        if args.fps is None:
            args.fps = 30.0
        cap.set(cv2.CAP_PROP_FRAME_WIDTH, args.width)
        cap.set(cv2.CAP_PROP_FRAME_HEIGHT, args.height)
        cap.set(cv2.CAP_PROP_FPS, args.fps)

    mode = f"JPEG q={args.quality}" if args.compressed else "raw BGR"
    src_desc = f"video {args.video}" if args.video else "camera 0"
    print(f"relink_camera_pub_raw: publishing {mode} frames at {args.width}x{args.height}, "
          f"target {args.fps} fps, from {src_desc}, on {TOPIC_CAMERA}")

    period = 1.0 / args.fps
    frame_id = 0
    sent_bytes = 0
    start = time.time()
    next_send = start
    consecutive_failures = 0
    while True:
        ok, frame = cap.read()
        if not ok:
            if args.video:
                # Loop the file at EOF instead of treating it as failure.
                cap.set(cv2.CAP_PROP_POS_FRAMES, 0)
                continue
            consecutive_failures += 1
            if consecutive_failures == 30:
                print("relink_camera_pub_raw: camera reads failing repeatedly "
                      "(device busy/still initializing?), still retrying...", file=sys.stderr)
            time.sleep(0.05)
            continue
        consecutive_failures = 0

        if args.width and (frame.shape[1], frame.shape[0]) != (args.width, args.height):
            frame = cv2.resize(frame, (args.width, args.height))

        if args.compressed:
            ok, jpeg = cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, args.quality])
            body = jpeg.tobytes()
        else:
            body = frame.tobytes()

        payload = struct.pack("<d", time.time()) + body
        node.publish_image(TOPIC_CAMERA, payload, frame_id)
        sent_bytes += len(payload)

        node.spin_once()
        frame_id += 1
        if frame_id % 30 == 0:
            elapsed = time.time() - start
            print(f"relink: {frame_id} frames, {frame_id/elapsed:.1f} fps, "
                  f"{sent_bytes/elapsed/1e6:.2f} MB/s, {len(payload)} bytes/frame")

        next_send += period
        sleep_left = next_send - time.time()
        if sleep_left > 0:
            time.sleep(sleep_left)


if __name__ == "__main__":
    sys.exit(main() or 0)

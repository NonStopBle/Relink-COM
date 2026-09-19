#!/usr/bin/env python3
"""ReLink camera subscriber counterpart to relink_camera_pub_raw.py --
displays the live feed in a cv2 window and prints running latency/FPS/
bandwidth stats, in the same format as ros2_camera_sub.py, so the two
printed stat streams can be compared directly. Auto-detects raw vs
JPEG-compressed payloads (a JPEG frame decodes; a raw frame doesn't and
falls back to reshaping by --width/--height), so the same subscriber
works against either publisher mode.

Requires OpenCV: pip install opencv-python

Run:
    python3 relink_camera_sub_raw.py [rlcore_ip] [--width W] [--height H] [--compressed]
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
    import numpy as np
except ImportError:
    print("relink_camera_sub_raw.py requires OpenCV -- pip install opencv-python", file=sys.stderr)
    sys.exit(1)

TOPIC_CAMERA = "/compare/camera"
HEADER_LEN = 8  # struct 'd'


def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("rlcore_ip", nargs="?", default=None)
    p.add_argument("--width", type=int, default=320)
    p.add_argument("--height", type=int, default=240)
    p.add_argument("--compressed", action="store_true")
    return p.parse_args()


def main():
    args = parse_args()

    node = RelinkNode()
    if args.rlcore_ip:
        node.set_rlcore.ip(args.rlcore_ip)
    else:
        node.use_multicast_discovery()

    recv_bytes = 0
    recv_count = 0
    latencies = []
    start = time.time()

    def on_frame(frame_id, data):
        nonlocal recv_bytes, recv_count
        send_time, = struct.unpack("<d", data[:HEADER_LEN])
        latency_ms = (time.time() - send_time) * 1000.0
        latencies.append(latency_ms)

        body = data[HEADER_LEN:]
        if args.compressed:
            arr = cv2.imdecode(np.frombuffer(body, dtype=np.uint8), cv2.IMREAD_COLOR)
        else:
            arr = np.frombuffer(body, dtype=np.uint8).reshape((args.height, args.width, 3))
        cv2.imshow("ReLink camera", arr)
        cv2.waitKey(1)

        recv_bytes += len(data)
        recv_count += 1
        if recv_count % 30 == 0:
            elapsed = time.time() - start
            avg_lat = sum(latencies[-30:]) / len(latencies[-30:])
            print(f"relink: {recv_count} frames, {recv_count/elapsed:.1f} fps, "
                  f"{recv_bytes/elapsed/1e6:.2f} MB/s, avg latency {avg_lat:.2f} ms, "
                  f"{len(data)} bytes/frame")

    node.subscribe_image(TOPIC_CAMERA, on_frame)
    print(f"relink_camera_sub_raw: subscribed to {TOPIC_CAMERA}, displaying live "
          "(ctrl-c to quit)")
    node.spin()


if __name__ == "__main__":
    sys.exit(main() or 0)

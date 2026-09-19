#!/usr/bin/env python3
"""custom_types_pubsub -- one node publishing and subscribing several
different message types at once: a built-in type (Bool), and two
user-defined custom types (Pose2D, a small struct; and a fixed-size
array-of-floats type). Wire-compatible with cpp/examples/custom_types_pubsub.cpp
-- run one copy of each and they talk to each other with zero changes.

Run (in two terminals, or on two machines):
    python3 custom_types_pubsub.py
"""
import ctypes
import math
import sys
import os
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from relink import RelinkNode, Bool

# --- user-defined message types -- these are the caller's own code,
# not part of the ReLink library. _pack_ = 1 keeps the layout identical
# to the C++ side's #pragma pack(1) equivalent (Step 7). ---


class Pose2D(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("x", ctypes.c_float), ("y", ctypes.c_float), ("theta", ctypes.c_float),
        ("seq", ctypes.c_uint32),
    ]


class Waypoints(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("x", ctypes.c_float * 4), ("y", ctypes.c_float * 4),
        ("count", ctypes.c_uint8),
    ]


TOPIC_ARMED = "/relink/armed"
TOPIC_POSE = "/relink/pose"
TOPIC_PATH = "/relink/path"


def main():
    node = RelinkNode()
    node.use_multicast_discovery()   # zero setup -- see Step 3

    # Subscribe to all three before advertising any of them, so an
    # early message from a peer that started first is never missed.
    node.subscribe(TOPIC_ARMED, Bool, lambda msg: print(f"armed:  {bool(msg.data)}"))
    node.subscribe(TOPIC_POSE, Pose2D, lambda msg: print(
        f"pose:   seq={msg.seq} x={msg.x:.2f} y={msg.y:.2f} theta={msg.theta:.2f}"))
    node.subscribe(TOPIC_PATH, Waypoints, lambda msg: print(
        f"path:   {msg.count} point(s), first=({msg.x[0]:.2f}, {msg.y[0]:.2f})"))

    node.advertise(TOPIC_ARMED, Bool)
    node.advertise(TOPIC_POSE, Pose2D)
    node.advertise(TOPIC_PATH, Waypoints)

    seq = 0
    while True:
        node.spin_once()   # services discovery -- call this every loop

        # Bool: toggles once every 10 iterations, just to show it moving.
        node.publish(TOPIC_ARMED, Bool(data=1 if (seq // 10) % 2 == 0 else 0))

        # Pose2D: a small circle, so the numbers visibly change every tick.
        t = seq * 0.1
        pose = Pose2D(x=math.cos(t), y=math.sin(t), theta=t, seq=seq)
        node.publish(TOPIC_POSE, pose)

        # Waypoints: a fixed-size array message -- the whole struct,
        # arrays included, is one flat blit on the wire, same as any
        # other trivially-copyable type.
        path = Waypoints()
        path.count = 4
        for i in range(4):
            path.x[i] = float(i)
            path.y[i] = float(i * i)
        node.publish(TOPIC_PATH, path)

        time.sleep(0.5)
        seq += 1


if __name__ == "__main__":
    main()

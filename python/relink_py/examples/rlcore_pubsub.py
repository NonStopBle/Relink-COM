#!/usr/bin/env python3
"""ReLink usage example -- publisher + subscriber, mode A (rlcore).
Python equivalent of relink_example.cpp: same topics, same message
shapes, same discovery mode, to demonstrate the two bindings talk the
same wire protocol.

Two processes: run with argv[1] == "pub" or "sub".
"""
import ctypes
import sys
import os
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from relink import RelinkNode, Float32

# --- user's own message definition (their own code, not part of the
# ReLink library itself -- mirrors ImuReading from relink_example.cpp) ---


class ImuReading(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("accel_x", ctypes.c_float), ("accel_y", ctypes.c_float), ("accel_z", ctypes.c_float),
        ("gyro_x", ctypes.c_float), ("gyro_y", ctypes.c_float), ("gyro_z", ctypes.c_float),
        ("timestamp_us", ctypes.c_uint64),
    ]


TOPIC_IMU = 100
TOPIC_TEMP = 101


def now_us() -> int:
    return int(time.time() * 1_000_000)


def main():
    if len(sys.argv) < 2:
        print(f"usage: {sys.argv[0]} [pub|sub] [rlcore_ip]", file=sys.stderr)
        return 1
    is_publisher = sys.argv[1] == "pub"
    rlcore_ip = sys.argv[2] if len(sys.argv) > 2 else "10.0.0.5"

    node = RelinkNode()
    node.set_rlcore.ip(rlcore_ip)
    # node.set_rlcore.port(9000)  # only if rlcore uses a non-default port

    if is_publisher:
        node.advertise(TOPIC_IMU, ImuReading)
        node.advertise(TOPIC_TEMP, Float32)
        print("publisher: advertising TOPIC_IMU and TOPIC_TEMP")

        i = 0
        while True:
            reading = ImuReading(
                accel_x=0.01 * i, accel_y=0.02, accel_z=9.81,
                gyro_x=0.0, gyro_y=0.0, gyro_z=0.0,
                timestamp_us=now_us(),
            )
            node.publish(TOPIC_IMU, reading)

            if i % 100 == 0:
                node.publish(TOPIC_TEMP, Float32(data=36.6))

            node.spin_once()
            time.sleep(0.001)
            i += 1
    else:
        def on_imu(msg: ImuReading):
            print(f"imu: accel=({msg.accel_x:.3f}, {msg.accel_y:.3f}, {msg.accel_z:.3f}) t={msg.timestamp_us}")

        def on_temp(msg: Float32):
            print(f"temp: {msg.data:.1f} C")

        node.subscribe(TOPIC_IMU, ImuReading, on_imu)
        node.subscribe(TOPIC_TEMP, Float32, on_temp)
        print("subscriber: waiting for messages on TOPIC_IMU and TOPIC_TEMP")
        node.spin()

    return 0


if __name__ == "__main__":
    sys.exit(main())

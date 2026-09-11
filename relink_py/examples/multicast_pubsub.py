#!/usr/bin/env python3
"""ReLink usage example -- publisher + subscriber, mode B (multicast,
no daemon). Same shape as comcore_pubsub.py but with
node.use_multicast_discovery() instead of set_com_core.

Two processes: run with argv[1] == "pub" or "sub".
"""
import ctypes
import sys
import os
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from relink import RelinkNode, Int32

TOPIC_COUNTER = 300


def main():
    if len(sys.argv) < 2:
        print(f"usage: {sys.argv[0]} [pub|sub]", file=sys.stderr)
        return 1
    is_publisher = sys.argv[1] == "pub"

    node = RelinkNode()
    node.use_multicast_discovery()

    if is_publisher:
        node.advertise(TOPIC_COUNTER, Int32)
        print("publisher: advertising TOPIC_COUNTER via multicast discovery")
        i = 0
        while True:
            node.publish(TOPIC_COUNTER, Int32(data=i))
            node.spin_once()
            time.sleep(0.1)
            i += 1
    else:
        def on_counter(msg: Int32):
            print(f"counter: {msg.data}")

        node.subscribe(TOPIC_COUNTER, Int32, on_counter)
        print("subscriber: waiting for messages on TOPIC_COUNTER via multicast discovery")
        node.spin()

    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Cross-language interop test -- publisher side, Python. Mirrors
tests/two_process_pub.cpp exactly (same topic/count/protocol) so it can
be paired with either the C++ or Python subscriber to prove wire-level
interop, not just com-core-registration interop.

usage: two_process_pub.py <comcore|multicast> [comcore_ip] <topic_id> <count>
"""
import sys
import time

sys.path.insert(0, "..")
from relink import RelinkNode, Int32


def main():
    if len(sys.argv) < 4:
        print(f"usage: {sys.argv[0]} <comcore|multicast> [comcore_ip] <topic_id> <count>", file=sys.stderr)
        return 2

    node = RelinkNode()
    argi = 1
    mode = sys.argv[argi]; argi += 1
    if mode == "comcore":
        ip = sys.argv[argi]; argi += 1
        node.set_com_core.ip(ip)
    elif mode == "multicast":
        node.use_multicast_discovery()
    else:
        print(f"unknown mode: {mode}", file=sys.stderr)
        return 2

    topic_id = int(sys.argv[argi]); argi += 1
    count = int(sys.argv[argi]); argi += 1

    node.advertise(topic_id, Int32)
    node.spin_once()
    time.sleep(1.5)

    peers = node.peers_for_topic(topic_id)
    print(f"pub: resolved {len(peers)} peer(s) for topic {topic_id}")

    for i in range(count):
        node.publish(topic_id, Int32(data=i))
        time.sleep(0.002)

    print(f"pub: done, sent {count} messages")
    time.sleep(0.5)
    return 0


if __name__ == "__main__":
    sys.exit(main())

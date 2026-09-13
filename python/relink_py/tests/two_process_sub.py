#!/usr/bin/env python3
"""Cross-language interop test -- subscriber side, Python. Mirrors
tests/two_process_sub.cpp exactly.

usage: two_process_sub.py <rlcore|multicast> [rlcore_ip] <topic_id> <expected_count> <timeout_sec>
"""
import sys
import threading
import time

sys.path.insert(0, "..")
from relink import RelinkNode, Int32


def main():
    if len(sys.argv) < 5:
        print(f"usage: {sys.argv[0]} <rlcore|multicast> [rlcore_ip] <topic_id> <expected_count> <timeout_sec>",
              file=sys.stderr)
        return 2

    node = RelinkNode()
    argi = 1
    mode = sys.argv[argi]; argi += 1
    if mode == "rlcore":
        ip = sys.argv[argi]; argi += 1
        node.set_rlcore.ip(ip)
    elif mode == "multicast":
        node.use_multicast_discovery()
    else:
        print(f"unknown mode: {mode}", file=sys.stderr)
        return 2

    topic_id = int(sys.argv[argi]); argi += 1
    expected_count = int(sys.argv[argi]); argi += 1
    timeout_sec = float(sys.argv[argi]); argi += 1

    lock = threading.Lock()
    received = set()

    def on_msg(msg: Int32):
        with lock:
            received.add(msg.data)

    node.subscribe(topic_id, Int32, on_msg)
    node.spin_once()

    deadline = time.time() + timeout_sec
    while time.time() < deadline:
        with lock:
            if len(received) >= expected_count:
                break
        time.sleep(0.02)

    with lock:
        all_present = len(received) == expected_count and all(i in received for i in range(expected_count))
        count = len(received)

    print(f"sub: received {count}/{expected_count} unique messages")
    if all_present:
        print("RESULT: PASS")
        return 0
    else:
        print("RESULT: FAIL")
        return 1


if __name__ == "__main__":
    sys.exit(main())

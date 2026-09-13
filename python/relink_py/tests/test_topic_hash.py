"""Unit tests for named-topic hashing (topic_hash.py) and RelinkNode's
string-topic registry (_topic_id_for/topic_name_for/rltopic_list).
Mirrors tests/test_topic_hash.cpp -- same fixtures, including the same
real (brute-force-found) hash collision, so both languages are checked
against the same ground truth.
"""

import ctypes
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from relink.topic_hash import fnv1a32
from relink.wire import NAT_PUNCH_TOPIC_ID
from relink.node import RelinkNode

failures = 0


def check(cond, desc):
    global failures
    if cond:
        print(f"ok: {desc}")
    else:
        print(f"FAIL: {desc}", file=sys.stderr)
        failures += 1


def check_throws(fn, desc):
    global failures
    try:
        fn()
        print(f"FAIL: expected exception: {desc}", file=sys.stderr)
        failures += 1
    except RuntimeError:
        print(f"ok: throws: {desc}")


class Msg(ctypes.Structure):
    _pack_ = 1
    _fields_ = [("x", ctypes.c_int32)]


# --- fnv1a32: deterministic, distinct names diverge, matches the C++ side ---
check(fnv1a32("/relink/lidar") == fnv1a32("/relink/lidar"), 'fnv1a32("/relink/lidar") is deterministic')
check(fnv1a32("/relink/lidar") != fnv1a32("/relink/camera/front"), "distinct names diverge")
check(fnv1a32("") == 0x811C9DC5, "empty string is just the FNV offset basis")

# --- _topic_id_for: same name -> same id, distinct names -> distinct ids ---
node = RelinkNode()
id1 = node._topic_id_for("/relink/camera/front")
id2 = node._topic_id_for("/relink/camera/front")
id3 = node._topic_id_for("/relink/camera/back")
check(id1 == id2, "same name -> same id")
check(id1 != id3, "distinct names -> distinct ids")
check(id1 == fnv1a32("/relink/camera/front"), "id matches raw fnv1a32")

# --- topic_name_for: reverse lookup via the local registry ---
node2 = RelinkNode()
tid = node2._topic_id_for("/relink/imu")
check(node2.topic_name_for(tid) == "/relink/imu", "topic_name_for resolves a known id")
check(node2.topic_name_for(tid + 1) == "", "unknown id resolves to empty string")

# --- reserved NAT-punch id guard: sanity-check the common (non-reserved)
# path. The reject branch (a name whose hash equals the reserved id) is
# not independently exercised here -- see test_topic_hash.cpp's note on
# why brute-forcing a real colliding string isn't worth the CPU time. ---
node3 = RelinkNode()
try:
    node3._topic_id_for("/relink/definitely-not-reserved")
    print("ok: ordinary name does not throw")
except RuntimeError:
    print("FAIL: ordinary name should not throw", file=sys.stderr)
    failures += 1
check(fnv1a32("/relink/definitely-not-reserved") != NAT_PUNCH_TOPIC_ID,
      "fixture name does not itself hash to the reserved id")

# --- genuine hash collision: two different real strings that hash to the
# SAME 32-bit id (found by brute-force search, not synthetic) -- shared
# fixture with test_topic_hash.cpp. Registering both under the same node
# must raise. ---
a = "/relink/topic/162789"
b = "/relink/topic/379192"
check(fnv1a32(a) == fnv1a32(b), "fixture strings really do collide")
node4 = RelinkNode()
node4._topic_id_for(a)
check_throws(lambda: node4._topic_id_for(b), "registering a second name that collides with an existing one")

# --- rltopic_list: reflects declared topics with names when known, empty
# name for numerically-declared topics ---
node5 = RelinkNode()
node5.advertise("/relink/lidar", Msg)
node5.subscribe(4242, Msg, lambda m: None)  # numeric, no name

topics = node5.rltopic_list()
check(len(topics) == 2, "rltopic_list has one entry per declared topic")
named = next((t for t in topics if t["topic_id"] == fnv1a32("/relink/lidar")), None)
numeric = next((t for t in topics if t["topic_id"] == 4242), None)
check(named is not None and named["name"] == "/relink/lidar", "named topic keeps its name")
check(numeric is not None and numeric["name"] == "", "numeric topic has an empty name")

# --- string and int topic args must resolve to the same wire id ---
node_a = RelinkNode()
node_b = RelinkNode()
node_a.advertise("/relink/lidar", Msg)
node_b.advertise(fnv1a32("/relink/lidar"), Msg)
check(node_a.rltopic_list()[0]["topic_id"] == node_b.rltopic_list()[0]["topic_id"],
      "string and numeric topic args agree on the wire id")

print()
if failures == 0:
    print("ALL PASS")
    sys.exit(0)
else:
    print(f"{failures} FAILURE(S)")
    sys.exit(1)

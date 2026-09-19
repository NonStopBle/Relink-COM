#!/usr/bin/env python3
"""std_msgs_pubsub -- publishes and subscribes every std_msgs composite
type ReLink ships (relink/standard_msgs.py), one topic per type: Empty,
Time, Duration, ColorRGBA, Header, String. Wire-compatible with
cpp/examples/std_msgs_pubsub.cpp.

(The std_msgs primitives -- Bool, Int32, Float32, etc. -- are covered
separately by builtin_types_pubsub.py, since they live in wire.py
rather than standard_msgs.py.)

Also demonstrates std_msgs's *MultiArray family (ROS's ByteMultiArray,
Int8/16/32/64MultiArray, UInt8/16/32/64MultiArray, Float32/64MultiArray
-- 11 types). standard_msgs.py does NOT ship these as ready-made
structs: it only documents the underlying length-prefixed layout
(wire.py's MultiArrayHeader) and says to build the concrete type
yourself. That is exactly what make_multiarray() below does -- a
fixed-capacity count-prefixed array, the same pattern standard_msgs.py
itself uses for PoseArray/Polygon/etc -- so this file both defines and
exercises the whole family, byte-compatible with the C++ side's
MultiArray<T, Cap> template.

Run (in two terminals, or on two machines):
    python3 std_msgs_pubsub.py
"""
import ctypes
import sys
import os
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from relink import RelinkNode
from relink import std_msgs

# --- std_msgs.*MultiArray family, user-defined (see file docstring) ---
# 8 elements is plenty to demonstrate the shape; raise MULTIARRAY_CAP
# if your own use needs more, same tradeoff as any other fixed-cap
# array type in this codebase (PoseArray, Polygon, ...).
MULTIARRAY_CAP = 8


def _multiarray(name, ctype):
    return type(name, (ctypes.LittleEndianStructure,), {
        "_pack_": 1,
        "_fields_": [("count", ctypes.c_uint32), ("data", ctype * MULTIARRAY_CAP)],
    })


# ByteMultiArray and UInt8MultiArray are distinct Python classes here
# (unlike the C++ side, where they're the same template instantiation)
# but share an identical byte layout -- both wrap c_uint8, matching
# ROS's own choice to keep Byte/UInt8 as separate named types over an
# identical wire shape (wire.py).
ByteMultiArray = _multiarray("ByteMultiArray", ctypes.c_uint8)
Int8MultiArray = _multiarray("Int8MultiArray", ctypes.c_int8)
Int16MultiArray = _multiarray("Int16MultiArray", ctypes.c_int16)
Int32MultiArray = _multiarray("Int32MultiArray", ctypes.c_int32)
Int64MultiArray = _multiarray("Int64MultiArray", ctypes.c_int64)
UInt8MultiArray = _multiarray("UInt8MultiArray", ctypes.c_uint8)
UInt16MultiArray = _multiarray("UInt16MultiArray", ctypes.c_uint16)
UInt32MultiArray = _multiarray("UInt32MultiArray", ctypes.c_uint32)
UInt64MultiArray = _multiarray("UInt64MultiArray", ctypes.c_uint64)
Float32MultiArray = _multiarray("Float32MultiArray", ctypes.c_float)
Float64MultiArray = _multiarray("Float64MultiArray", ctypes.c_double)


def make_multiarray(cls, i):
    m = cls()
    m.count = MULTIARRAY_CAP
    for k in range(MULTIARRAY_CAP):
        m.data[k] = i + k
    return m


def main():
    node = RelinkNode()
    node.use_multicast_discovery()   # zero setup -- see Step 3

    node.subscribe("/relink/std/empty", std_msgs.Empty, lambda m: print("std_msgs/Empty received"))
    node.subscribe("/relink/std/time", std_msgs.Time, lambda m: print(f"std_msgs/Time     sec={m.sec} nsec={m.nsec}"))
    node.subscribe("/relink/std/duration", std_msgs.Duration, lambda m: print(f"std_msgs/Duration sec={m.sec} nsec={m.nsec}"))
    node.subscribe("/relink/std/color", std_msgs.ColorRGBA, lambda m: print(f"std_msgs/ColorRGBA r={m.r:.2f} g={m.g:.2f} b={m.b:.2f} a={m.a:.2f}"))
    node.subscribe("/relink/std/header", std_msgs.Header, lambda m: print(f"std_msgs/Header   seq={m.seq} frame={m.frame_id_str()}"))
    node.subscribe("/relink/std/string", std_msgs.String, lambda m: print(f"std_msgs/String   \"{m.str()}\""))

    node.subscribe("/relink/std/multiarray/byte", ByteMultiArray, lambda m: print(f"std_msgs/ByteMultiArray    count={m.count} [0]={m.data[0]}"))
    node.subscribe("/relink/std/multiarray/int8", Int8MultiArray, lambda m: print(f"std_msgs/Int8MultiArray    count={m.count} [0]={m.data[0]}"))
    node.subscribe("/relink/std/multiarray/int16", Int16MultiArray, lambda m: print(f"std_msgs/Int16MultiArray   count={m.count} [0]={m.data[0]}"))
    node.subscribe("/relink/std/multiarray/int32", Int32MultiArray, lambda m: print(f"std_msgs/Int32MultiArray   count={m.count} [0]={m.data[0]}"))
    node.subscribe("/relink/std/multiarray/int64", Int64MultiArray, lambda m: print(f"std_msgs/Int64MultiArray   count={m.count} [0]={m.data[0]}"))
    node.subscribe("/relink/std/multiarray/uint8", UInt8MultiArray, lambda m: print(f"std_msgs/UInt8MultiArray   count={m.count} [0]={m.data[0]}"))
    node.subscribe("/relink/std/multiarray/uint16", UInt16MultiArray, lambda m: print(f"std_msgs/UInt16MultiArray  count={m.count} [0]={m.data[0]}"))
    node.subscribe("/relink/std/multiarray/uint32", UInt32MultiArray, lambda m: print(f"std_msgs/UInt32MultiArray  count={m.count} [0]={m.data[0]}"))
    node.subscribe("/relink/std/multiarray/uint64", UInt64MultiArray, lambda m: print(f"std_msgs/UInt64MultiArray  count={m.count} [0]={m.data[0]}"))
    node.subscribe("/relink/std/multiarray/float32", Float32MultiArray, lambda m: print(f"std_msgs/Float32MultiArray count={m.count} [0]={m.data[0]:.2f}"))
    node.subscribe("/relink/std/multiarray/float64", Float64MultiArray, lambda m: print(f"std_msgs/Float64MultiArray count={m.count} [0]={m.data[0]:.2f}"))

    node.advertise("/relink/std/empty", std_msgs.Empty)
    node.advertise("/relink/std/time", std_msgs.Time)
    node.advertise("/relink/std/duration", std_msgs.Duration)
    node.advertise("/relink/std/color", std_msgs.ColorRGBA)
    node.advertise("/relink/std/header", std_msgs.Header)
    node.advertise("/relink/std/string", std_msgs.String)

    node.advertise("/relink/std/multiarray/byte", ByteMultiArray)
    node.advertise("/relink/std/multiarray/int8", Int8MultiArray)
    node.advertise("/relink/std/multiarray/int16", Int16MultiArray)
    node.advertise("/relink/std/multiarray/int32", Int32MultiArray)
    node.advertise("/relink/std/multiarray/int64", Int64MultiArray)
    node.advertise("/relink/std/multiarray/uint8", UInt8MultiArray)
    node.advertise("/relink/std/multiarray/uint16", UInt16MultiArray)
    node.advertise("/relink/std/multiarray/uint32", UInt32MultiArray)
    node.advertise("/relink/std/multiarray/uint64", UInt64MultiArray)
    node.advertise("/relink/std/multiarray/float32", Float32MultiArray)
    node.advertise("/relink/std/multiarray/float64", Float64MultiArray)

    i = 0
    while True:
        node.spin_once()   # services discovery -- call this every loop

        node.publish("/relink/std/empty", std_msgs.Empty())
        node.publish("/relink/std/time", std_msgs.Time.now())
        node.publish("/relink/std/duration", std_msgs.Duration(sec=i, nsec=0))
        node.publish("/relink/std/color", std_msgs.ColorRGBA(r=1.0, g=0.5, b=0.0, a=1.0))

        h = std_msgs.Header()
        h.seq = i
        h.stamp_now()
        h.set_frame_id("base_link")
        node.publish("/relink/std/header", h)

        s = std_msgs.String()
        s.set("hello from std_msgs_pubsub")
        node.publish("/relink/std/string", s)

        node.publish("/relink/std/multiarray/byte", make_multiarray(ByteMultiArray, i))
        node.publish("/relink/std/multiarray/int8", make_multiarray(Int8MultiArray, i % 100))
        node.publish("/relink/std/multiarray/int16", make_multiarray(Int16MultiArray, i))
        node.publish("/relink/std/multiarray/int32", make_multiarray(Int32MultiArray, i))
        node.publish("/relink/std/multiarray/int64", make_multiarray(Int64MultiArray, i))
        node.publish("/relink/std/multiarray/uint8", make_multiarray(UInt8MultiArray, i % 200))
        node.publish("/relink/std/multiarray/uint16", make_multiarray(UInt16MultiArray, i))
        node.publish("/relink/std/multiarray/uint32", make_multiarray(UInt32MultiArray, i))
        node.publish("/relink/std/multiarray/uint64", make_multiarray(UInt64MultiArray, i))
        node.publish("/relink/std/multiarray/float32", make_multiarray(Float32MultiArray, i))
        node.publish("/relink/std/multiarray/float64", make_multiarray(Float64MultiArray, i))

        time.sleep(0.5)
        i += 1


if __name__ == "__main__":
    main()

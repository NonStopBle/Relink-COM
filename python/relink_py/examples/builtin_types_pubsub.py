#!/usr/bin/env python3
"""builtin_types_pubsub -- publishes and subscribes every built-in
message type ReLink ships (relink/wire.py), one topic per type, so
this file doubles as a runnable reference list. Wire-compatible with
cpp/examples/builtin_types_pubsub.cpp.

These are the `std_msgs`-style default types available with no
definition of your own needed -- see custom_types_pubsub.py for
user-defined types instead.

Built-in types (13 total), each wrapping a single `data` field:
    Bool     UInt8
    Byte     UInt16
    Char     UInt32
    Int8     UInt64
    Int16    Float32
    Int32    Float64
    Int64

Run (in two terminals, or on two machines):
    python3 builtin_types_pubsub.py
"""
import sys
import os
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from relink import (
    RelinkNode,
    Bool, Byte, Char, Int8, Int16, Int32, Int64,
    UInt8, UInt16, UInt32, UInt64, Float32, Float64,
)

# (topic, type, format string) -- one row per built-in type
TYPES = [
    ("/relink/types/bool", Bool, "Bool     {}"),
    ("/relink/types/byte", Byte, "Byte     {}"),
    ("/relink/types/char", Char, "Char     '{}'"),
    ("/relink/types/int8", Int8, "Int8     {}"),
    ("/relink/types/int16", Int16, "Int16    {}"),
    ("/relink/types/int32", Int32, "Int32    {}"),
    ("/relink/types/int64", Int64, "Int64    {}"),
    ("/relink/types/uint8", UInt8, "UInt8    {}"),
    ("/relink/types/uint16", UInt16, "UInt16   {}"),
    ("/relink/types/uint32", UInt32, "UInt32   {}"),
    ("/relink/types/uint64", UInt64, "UInt64   {}"),
    ("/relink/types/float32", Float32, "Float32  {:.3f}"),
    ("/relink/types/float64", Float64, "Float64  {:.6f}"),
]


def main():
    node = RelinkNode()
    node.use_multicast_discovery()   # zero setup -- see Step 3

    # Subscribe before advertising, so an early message from a peer
    # that started first is never missed.
    for topic, msg_type, fmt in TYPES:
        def make_cb(fmt=fmt):
            if "'{}'" in fmt:
                return lambda m: print(fmt.format(chr(m.data)))
            return lambda m: print(fmt.format(m.data))
        node.subscribe(topic, msg_type, make_cb())

    for topic, msg_type, _ in TYPES:
        node.advertise(topic, msg_type)

    i = 0
    while True:
        node.spin_once()   # services discovery -- call this every loop

        node.publish("/relink/types/bool", Bool(data=i % 2))
        node.publish("/relink/types/byte", Byte(data=i % 256))
        node.publish("/relink/types/char", Char(data=(ord('A') + i % 26)))
        node.publish("/relink/types/int8", Int8(data=-(i % 128)))
        node.publish("/relink/types/int16", Int16(data=-i))
        node.publish("/relink/types/int32", Int32(data=-i))
        node.publish("/relink/types/int64", Int64(data=-i))
        node.publish("/relink/types/uint8", UInt8(data=i % 256))
        node.publish("/relink/types/uint16", UInt16(data=i))
        node.publish("/relink/types/uint32", UInt32(data=i))
        node.publish("/relink/types/uint64", UInt64(data=i))
        node.publish("/relink/types/float32", Float32(data=i * 0.5))
        node.publish("/relink/types/float64", Float64(data=i * 0.25))

        time.sleep(0.5)
        i += 1


if __name__ == "__main__":
    main()

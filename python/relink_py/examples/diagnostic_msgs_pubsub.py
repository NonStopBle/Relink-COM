#!/usr/bin/env python3
"""diagnostic_msgs_pubsub -- publishes and subscribes every
diagnostic_msgs type ReLink ships (relink/standard_msgs.py), one topic
per type: KeyValue, DiagnosticStatus, DiagnosticArray. Wire-compatible
with cpp/examples/diagnostic_msgs_pubsub.cpp.

Run (in two terminals, or on two machines):
    python3 diagnostic_msgs_pubsub.py
"""
import sys
import os
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from relink import RelinkNode
from relink import diagnostic_msgs


def main():
    node = RelinkNode()
    node.use_multicast_discovery()   # zero setup -- see Step 3

    node.subscribe("/relink/diag/key_value", diagnostic_msgs.KeyValue, lambda m: print(f"diagnostic_msgs/KeyValue {m.key_str()}={m.value_str()}"))
    node.subscribe("/relink/diag/status", diagnostic_msgs.DiagnosticStatus, lambda m: print(f"diagnostic_msgs/DiagnosticStatus level={m.level} name={m.name_str()}"))
    node.subscribe("/relink/diag/array", diagnostic_msgs.DiagnosticArray, lambda m: print(f"diagnostic_msgs/DiagnosticArray status_count={m.status_count}"))

    node.advertise("/relink/diag/key_value", diagnostic_msgs.KeyValue)
    node.advertise("/relink/diag/status", diagnostic_msgs.DiagnosticStatus)
    node.advertise("/relink/diag/array", diagnostic_msgs.DiagnosticArray)

    i = 0
    while True:
        node.spin_once()   # services discovery -- call this every loop

        kv = diagnostic_msgs.KeyValue()
        kv.set_key("battery_pct")
        kv.set_value(str(100 - (i % 100)))
        node.publish("/relink/diag/key_value", kv)

        status = diagnostic_msgs.DiagnosticStatus()
        status.level = diagnostic_msgs.DiagnosticStatus.OK
        status.set_name("battery_monitor")
        status.set_message("nominal")
        status.set_hardware_id("bms_01")
        node.publish("/relink/diag/status", status)

        arr = diagnostic_msgs.DiagnosticArray()
        arr.header.set_frame_id("diagnostics")
        arr.status_count = 1
        arr.status[0].level = diagnostic_msgs.DiagnosticStatus.OK
        arr.status[0].set_name("battery_monitor")
        node.publish("/relink/diag/array", arr)

        time.sleep(0.5)
        i += 1


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""actionlib_msgs_pubsub -- publishes and subscribes every
actionlib_msgs type ReLink ships (relink/standard_msgs.py), one topic
per type: GoalID, GoalStatus, GoalStatusArray. Wire-compatible with
cpp/examples/actionlib_msgs_pubsub.cpp.

Run (in two terminals, or on two machines):
    python3 actionlib_msgs_pubsub.py
"""
import sys
import os
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from relink import RelinkNode
from relink import std_msgs, actionlib_msgs


def main():
    node = RelinkNode()
    node.use_multicast_discovery()   # zero setup -- see Step 3

    node.subscribe("/relink/action/goal_id", actionlib_msgs.GoalID, lambda m: print(f"actionlib_msgs/GoalID id={m.id_str()}"))
    node.subscribe("/relink/action/goal_status", actionlib_msgs.GoalStatus, lambda m: print(f"actionlib_msgs/GoalStatus status={m.status} text={m.text_str()}"))
    node.subscribe("/relink/action/goal_status_array", actionlib_msgs.GoalStatusArray, lambda m: print(f"actionlib_msgs/GoalStatusArray status_list_count={m.status_list_count}"))

    node.advertise("/relink/action/goal_id", actionlib_msgs.GoalID)
    node.advertise("/relink/action/goal_status", actionlib_msgs.GoalStatus)
    node.advertise("/relink/action/goal_status_array", actionlib_msgs.GoalStatusArray)

    i = 0
    while True:
        node.spin_once()   # services discovery -- call this every loop

        gid = actionlib_msgs.GoalID()
        gid.stamp = std_msgs.Time.now()
        gid.set_id(f"goal_{i}")
        node.publish("/relink/action/goal_id", gid)

        gs = actionlib_msgs.GoalStatus()
        gs.status = actionlib_msgs.GoalStatus.ACTIVE
        gs.set_text("moving to goal")
        node.publish("/relink/action/goal_status", gs)

        gsa = actionlib_msgs.GoalStatusArray()
        gsa.header.set_frame_id("action_server")
        gsa.status_list_count = 1
        gsa.status_list[0].status = actionlib_msgs.GoalStatus.ACTIVE
        node.publish("/relink/action/goal_status_array", gsa)

        time.sleep(0.5)
        i += 1


if __name__ == "__main__":
    main()

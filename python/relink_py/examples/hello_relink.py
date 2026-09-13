#!/usr/bin/env python3
"""hello_relink -- the simplest possible ReLink program.

Run this same script on two (or more) machines on the same network, or
in two terminal windows on one machine, and they will find each other
automatically (no setup, no config file, no separate daemon) and start
exchanging messages -- each copy is both a publisher and a subscriber,
sending a counter once a second and printing whatever it receives from
the others.

Run (in two terminals, or on two machines):
    python3 examples/hello_relink.py
"""
import sys
import os
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from relink import RelinkNode, Int32

# Every ReLink topic needs a wire id, but you don't have to hand-assign
# a number: give advertise/subscribe/publish a human-readable name
# instead and ReLink hashes it down to the uint32 that actually goes on
# the wire (see RelinkNode._topic_id_for in node.py). Both sides just
# need to type the same string -- no shared constant/header needed. A
# numeric id (e.g. `TOPIC_HELLO = 42`) still works exactly as before if
# you'd rather assign ids by hand.
TOPIC_HELLO = "/relink/hello"


def main():
    node = RelinkNode()

    # Multicast discovery: zero setup, no daemon to run first. See
    # rlcore_pubsub.py for the alternative (a small daemon, useful when
    # multicast isn't available on your network).
    node.use_multicast_discovery()

    # Subscribe first so we don't miss any early messages, then
    # advertise -- both calls just declare intent, nothing is sent yet.
    node.subscribe(TOPIC_HELLO, Int32, lambda msg: print(f"received: {msg.data}"))
    node.advertise(TOPIC_HELLO, Int32)

    # peers_for_topic() still takes the numeric wire id -- resolve the
    # name once via _topic_id_for() (idempotent: the same name always
    # returns the same id from the registry).
    topic_id = node._topic_id_for(TOPIC_HELLO)

    counter = 0
    while True:
        node.spin_once()  # services discovery; incoming messages are
                           # delivered on their own thread, no polling
                           # needed for receiving

        peers = node.peers_for_topic(topic_id)
        if not peers:
            # No other copy of this program has been found yet. This is
            # normal for the first second or two -- multicast discovery
            # finds peers via a short burst of "here I am" broadcasts on
            # startup, so give it a moment.
            print("(no peers found yet -- is another copy of this program running on the network?)")
        else:
            node.publish(TOPIC_HELLO, Int32(data=counter))
            print(f"sent:     {counter} (to {len(peers)} peer(s))")

        time.sleep(1)
        counter += 1


if __name__ == "__main__":
    main()

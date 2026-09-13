#!/usr/bin/env python3
"""Standalone UDP relay daemon (Python) -- companion to the C++
relink-relay for environments without a compiler. Forwards ReLink data
frames between clients registered for the same topic; see relink.hpp's
relay_wire.hpp and README Step 13 for why a relay reaches nodes that
direct peer-to-peer NAT punching structurally cannot.

Performance notes (the honest ceiling for pure-Python UDP forwarding,
no C extension): one recvfrom per iteration into a bytes object the
socket layer already had to allocate -- there's no way to avoid that
allocation from pure Python, unlike the C++ relay's reused stack
buffer. What IS avoided: no re-encoding of the frame (the exact bytes
recvfrom returned are handed straight to sendto for every destination,
never rebuilt), no per-packet topic lookup beyond one dict get, and no
per-packet time.time() call for TTL sweeping (that's batched, not
inline -- only the register/keepalive path pays for one).

usage: relink_relay.py [port]
"""
import socket
import sys
import time

sys.path.insert(0, __file__.rsplit("/", 2)[0] + "/relink_py")
from relink.relay_wire import RELAY_DEFAULT_PORT, decode_relay_register, peek_frame_topic_id

MEMBER_TTL_SECONDS = 30  # must outlive the client's re-register interval


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else RELAY_DEFAULT_PORT

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", port))
    print(f"relink-relay (python) listening on 0.0.0.0:{port}", flush=True)

    groups = {}  # topic_id -> {addr: last_seen}
    last_sweep = time.time()

    while True:
        try:
            data, addr = sock.recvfrom(1500)
        except OSError:
            continue
        now = time.time()

        topic_id = decode_relay_register(data)
        if topic_id is not None:
            groups.setdefault(topic_id, {})[addr] = now
        else:
            topic_id = peek_frame_topic_id(data)
            if topic_id is not None:
                members = groups.get(topic_id)
                if members:
                    for member_addr in members:
                        if member_addr == addr:
                            continue  # never echo back to the sender
                        try:
                            sock.sendto(data, member_addr)
                        except OSError:
                            pass
            # else: unrecognized bytes -- silently dropped, a relay must
            # never misinterpret bytes it can't identify.

        if now - last_sweep >= 1.0:
            last_sweep = now
            for topic_id in list(groups.keys()):
                members = groups[topic_id]
                stale = [a for a, seen in members.items() if now - seen > MEMBER_TTL_SECONDS]
                for a in stale:
                    del members[a]
                if not members:
                    del groups[topic_id]


if __name__ == "__main__":
    main()

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

usage: relink-relay [--port <port>] [--ip <address>] [-h|--help] [port]
"""
import socket
import sys
import time

from ..relay_wire import RELAY_DEFAULT_PORT, decode_relay_register, peek_frame_topic_id

MEMBER_TTL_SECONDS = 30  # must outlive the client's re-register interval


def print_usage():
    print(f"usage: relink-relay [--port <port>] [--ip <address>] [-h|--help] [port]\n"
          f"\n"
          f"  --port <port>    UDP port to listen on (default {RELAY_DEFAULT_PORT})\n"
          f"  --ip <address>   local address to bind to (default 0.0.0.0, all interfaces)\n"
          f"  [port]           positional shorthand for --port, kept for backward compatibility\n"
          f"  -h, --help       show this help and exit")


def parse_args(argv):
    """Returns (port, bind_ip). Exits the process on a usage error, same
    as rlcore.py's argument handling -- kept as plain sys.argv parsing
    (no argparse) to match that file's style, since both are small
    single-purpose daemons."""
    port = None
    bind_ip = "0.0.0.0"
    positional = []
    i = 0
    while i < len(argv):
        arg = argv[i]
        if arg == "--port" and i + 1 < len(argv):
            port = int(argv[i + 1])
            i += 2
        elif arg == "--ip" and i + 1 < len(argv):
            bind_ip = argv[i + 1]
            i += 2
        elif arg.startswith("-"):
            print(f"relink-relay: unrecognized argument '{arg}' (see --help)", file=sys.stderr)
            sys.exit(1)
        else:
            positional.append(arg)
            i += 1

    if positional:
        if len(positional) > 1:
            print(f"relink-relay: unrecognized argument '{positional[1]}' (see --help)", file=sys.stderr)
            sys.exit(1)
        if port is not None:
            print("relink-relay: give the port via --port or as a positional argument, not both",
                  file=sys.stderr)
            sys.exit(1)
        port = int(positional[0])

    return (RELAY_DEFAULT_PORT if port is None else port), bind_ip


def main():
    argv = sys.argv[1:]
    if "-h" in argv or "--help" in argv:
        print_usage()
        return

    port, bind_ip = parse_args(argv)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.bind((bind_ip, port))
    except OSError as e:
        print(f"relink-relay: could not bind to {bind_ip}:{port} -- {e}\n"
              f"relink-relay: another process may already be listening there "
              f"(try --port <other-port>, or check `ss -ulnp`)", file=sys.stderr)
        sys.exit(1)
    print(f"relink-relay (python) listening on {bind_ip}:{port}", flush=True)

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

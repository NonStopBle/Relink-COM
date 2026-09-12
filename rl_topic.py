#!/usr/bin/env python3
"""rl_topic -- a rostopic-style CLI for ReLink's topic name directory.

Two subcommands, mirroring rostopic's ergonomics:
    rl_topic.py list                     list every topic id/name this
                                          process could learn about
    rl_topic.py info <name-or-id>        show what's known about one topic

Where the names come from (see relink/include/relink/topic_directory.hpp
and relink_py/relink/topic_directory.py for the wire protocol):

  --rlcore-ip <ip> [--rlcore-port <port>]
      Query the rlcore daemon directly (unicast RLNQ). rlcore
      accumulates topic names from every node that has ever registered
      with it and announced names (RLNM) -- one query gets you the
      fleet-wide picture without reaching each node individually.

  (no --rlcore-ip given)
      Broadcast RLNQ to the multicast discovery group (239.255.0.1:7400
      by default) and collect RLNR replies from every node that answers
      within the timeout window. Only sees nodes that are (a) running,
      (b) using multicast discovery, and (c) reachable on this network
      segment -- there is no central registry to ask instead, by design
      (see the root README's "What ReLink actually is").

Results are cached to ~/.cache/relink/topic_names.json across runs, so a
topic name already learned once doesn't need to be re-asked for --
running `list` again reuses the cache immediately and only asks the
network/rlcore for anything new, unless --no-cache or --refresh is
passed.
"""
import argparse
import binascii
import json
import os
import socket
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "relink_py"))
from relink import topic_directory as tdir
from relink import RelinkNode

DEFAULT_MULTICAST_GROUP = "239.255.0.1"
DEFAULT_MULTICAST_PORT = 7400
DEFAULT_RLCORE_PORT = 8445
DEFAULT_TIMEOUT_S = 1.5

CACHE_PATH = os.path.expanduser("~/.cache/relink/topic_names.json")


def load_cache():
    try:
        with open(CACHE_PATH) as f:
            raw = json.load(f)
        return {int(k): v for k, v in raw.items()}
    except (FileNotFoundError, ValueError, json.JSONDecodeError):
        return {}


def save_cache(cache):
    os.makedirs(os.path.dirname(CACHE_PATH), exist_ok=True)
    with open(CACHE_PATH, "w") as f:
        json.dump({str(k): v for k, v in cache.items()}, f, indent=2, sort_keys=True)


def query_rlcore(ip: str, port: int, timeout: float):
    """Unicast RLNQ to rlcore, return {topic_id: name}."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    try:
        s.sendto(tdir.encode_query(), (ip, port))
        data, _ = s.recvfrom(tdir.MAX_PACKET)
    except (socket.timeout, OSError) as e:
        print(f"rl_topic: no reply from rlcore at {ip}:{port} ({e})", file=sys.stderr)
        return {}
    finally:
        s.close()

    if tdir.packet_kind(data) != "reply":
        print(f"rl_topic: unexpected reply from rlcore at {ip}:{port}", file=sys.stderr)
        return {}
    entries = tdir.decode_entries(data)
    if entries is None:
        print(f"rl_topic: malformed reply from rlcore at {ip}:{port}", file=sys.stderr)
        return {}
    return {e.topic_id: e.name for e in entries}


def query_multicast(group: str, port: int, timeout: float):
    """Broadcast RLNQ to the multicast group, collect every RLNR reply
    that arrives within `timeout` seconds. Returns {topic_id: name}."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.settimeout(timeout)
    found = {}
    try:
        s.sendto(tdir.encode_query(), (group, port))
        deadline = time.time() + timeout
        while True:
            remaining = deadline - time.time()
            if remaining <= 0:
                break
            s.settimeout(remaining)
            try:
                data, _addr = s.recvfrom(tdir.MAX_PACKET)
            except socket.timeout:
                break
            if tdir.packet_kind(data) != "reply":
                continue
            entries = tdir.decode_entries(data)
            if entries is None:
                continue
            for e in entries:
                found[e.topic_id] = e.name
    except OSError as e:
        print(f"rl_topic: multicast query failed ({e})", file=sys.stderr)
    finally:
        s.close()
    return found


def collect_names(args) -> dict:
    if args.rlcore_ip:
        fresh = query_rlcore(args.rlcore_ip, args.rlcore_port, args.timeout)
    else:
        fresh = query_multicast(args.group, args.port, args.timeout)

    if args.no_cache:
        return fresh

    cache = {} if args.refresh else load_cache()
    cache.update(fresh)  # fresh network data always wins over a stale cache entry
    save_cache(cache)
    return cache


def resolve_topic_arg(raw: str):
    """A numeric-looking argument is a wire id, anything else is a name
    to be hashed -- RelinkNode._topic_id_for() handles the hashing, this
    just avoids treating "12345" as the literal string to hash."""
    if raw.lstrip("-").isdigit():
        return int(raw)
    return raw


def make_node(args) -> RelinkNode:
    node = RelinkNode()
    if args.rlcore_ip:
        node.set_rlcore.ip(args.rlcore_ip)
        node.set_rlcore.port(args.rlcore_port)
    else:
        node.use_multicast_discovery()
    return node


def wait_for_peers(node: RelinkNode, topic_id: int, timeout: float) -> int:
    # Caller must have already called advertise_raw()/publish_raw()/
    # subscribe_raw() at least once (declaring the topic) before this --
    # the FIRST spin_once() anywhere snapshots the beacon/registration
    # topic list, so declaring it here would be too late.
    deadline = time.time() + timeout
    while time.time() < deadline:
        node.spin_once()
        n = len(node.peers_for_topic(topic_id))
        if n > 0:
            return n
        time.sleep(0.1)
    return len(node.peers_for_topic(topic_id))


def cmd_hz(args):
    """Mirrors `rostopic hz <topic>`: measures the publish rate observed
    on one topic by subscribing raw and timing arrivals -- ReLink has no
    message type registry, so (unlike rostopic) this can't validate the
    payload shape, only how often *something* arrives on this topic id."""
    node = make_node(args)
    topic = resolve_topic_arg(args.topic)
    topic_id = node._topic_id_for(topic)
    arrivals = []

    def on_msg(payload: bytes):
        arrivals.append(time.monotonic())

    node.subscribe_raw(topic, on_msg)
    node.spin_once()
    print(f"subscribed to topic id {topic_id} ({args.topic}), waiting for messages "
          f"(Ctrl-C to stop)...", file=sys.stderr)

    window = args.window
    try:
        while True:
            time.sleep(0.5)
            node.spin_once()
            cutoff = time.monotonic() - args.report_every
            recent = [t for t in arrivals if t >= cutoff]
            if len(recent) < 2:
                print("no new messages", file=sys.stderr)
                continue
            intervals = [b - a for a, b in zip(recent, recent[1:])]
            rate = len(recent) / (recent[-1] - recent[0]) if recent[-1] > recent[0] else 0.0
            mean_iv = sum(intervals) / len(intervals)
            var = sum((iv - mean_iv) ** 2 for iv in intervals) / len(intervals)
            std = var ** 0.5
            print(f"average rate: {rate:.3f}")
            print(f"\tmin: {min(intervals):.5f}s max: {max(intervals):.5f}s "
                  f"std dev: {std:.5f}s window: {min(len(recent), window)}")
            arrivals[:] = recent[-window:]
    except KeyboardInterrupt:
        pass


def cmd_bw(args):
    """Mirrors `rostopic bw <topic>`: measures bandwidth (bytes/sec) on
    one topic by subscribing raw and summing payload sizes."""
    node = make_node(args)
    topic = resolve_topic_arg(args.topic)
    topic_id = node._topic_id_for(topic)
    samples = []  # (monotonic_time, size)

    def on_msg(payload: bytes):
        samples.append((time.monotonic(), len(payload)))

    node.subscribe_raw(topic, on_msg)
    node.spin_once()
    print(f"subscribed to topic id {topic_id} ({args.topic}), waiting for messages "
          f"(Ctrl-C to stop)...", file=sys.stderr)

    try:
        while True:
            time.sleep(1.0)
            node.spin_once()
            cutoff = time.monotonic() - args.report_every
            recent = [s for s in samples if s[0] >= cutoff]
            if len(recent) < 2:
                print("no new messages", file=sys.stderr)
                continue
            total_bytes = sum(sz for _, sz in recent)
            span = recent[-1][0] - recent[0][0]
            bw = total_bytes / span if span > 0 else 0.0
            mean_sz = total_bytes / len(recent)
            print(f"average: {bw:.1f} B/s")
            print(f"\tmean: {mean_sz:.1f} B min: {min(sz for _, sz in recent)} B "
                  f"max: {max(sz for _, sz in recent)} B window: {len(recent)}")
            samples[:] = recent
    except KeyboardInterrupt:
        pass


def cmd_echo(args):
    """Mirrors `rostopic echo <topic>`: prints each message as it
    arrives. ReLink has no message-type registry to decode the payload
    against, so this prints raw hex bytes -- pipe through your own
    struct.unpack if you know the shape (see the root README's custom
    message type examples)."""
    node = make_node(args)
    topic = resolve_topic_arg(args.topic)
    topic_id = node._topic_id_for(topic)
    count = [0]

    def on_msg(payload: bytes):
        count[0] += 1
        print(f"--- #{count[0]} ({len(payload)} bytes) ---")
        print(binascii.hexlify(payload, " ").decode())
        if args.count and count[0] >= args.count:
            # Runs on RelinkNode's background data thread -- raising
            # SystemExit here would only kill that thread, not the
            # process, since the main thread is asleep in its own loop
            # below. os._exit() is the correct way to stop the whole
            # process from a non-main thread.
            sys.stdout.flush()
            os._exit(0)

    node.subscribe_raw(topic, on_msg)
    node.spin_once()
    print(f"subscribed to topic id {topic_id} ({args.topic}) (Ctrl-C to stop)...", file=sys.stderr)
    try:
        while True:
            time.sleep(0.05)
            node.spin_once()
    except KeyboardInterrupt:
        pass


def cmd_pub(args):
    """Mirrors `rostopic pub <topic> <data>`: publishes a raw payload.
    ReLink has no YAML message literal syntax (no type registry to parse
    against), so the payload is given as --hex or --text instead."""
    if args.hex is None and args.text is None:
        print("rl_topic pub: need --hex <hexbytes> or --text <string>", file=sys.stderr)
        sys.exit(2)
    payload = bytes.fromhex(args.hex) if args.hex is not None else args.text.encode("utf-8")

    node = make_node(args)
    topic = resolve_topic_arg(args.topic)
    topic_id = node._topic_id_for(topic)
    node.advertise_raw(topic)  # must declare BEFORE the first spin_once()
    n_peers = wait_for_peers(node, topic_id, args.timeout)
    if n_peers == 0:
        print(f"rl_topic pub: no peers found for topic id {topic_id} within {args.timeout}s "
              f"-- publishing anyway (no-op if truly no one is listening)", file=sys.stderr)

    reps = args.repeat if args.repeat else 1
    for i in range(reps):
        ok = node.publish_raw(topic, payload)
        print(f"published {len(payload)} bytes to topic id {topic_id} "
              f"({args.topic}): {'ok' if ok else 'no peers / failed'}")
        if i + 1 < reps:
            time.sleep(args.rate_period)


def cmd_list(args):
    names = collect_names(args)
    if not names:
        print("rl_topic: no topics found "
              f"({'rlcore ' + args.rlcore_ip if args.rlcore_ip else 'multicast'}, "
              f"{args.timeout}s timeout)")
        return
    width = max(len(str(tid)) for tid in names)
    for tid in sorted(names):
        name = names[tid]
        label = name if name else "(unnamed)"
        print(f"{tid:<{width}}  {label}")


def cmd_info(args):
    names = collect_names(args)
    query = args.topic
    matches = []
    if query.isdigit() or (query.startswith("-") and query[1:].isdigit()):
        tid = int(query)
        if tid in names:
            matches.append((tid, names[tid]))
    else:
        matches = [(tid, name) for tid, name in names.items() if name == query]

    if not matches:
        print(f"rl_topic: no known topic matches \"{query}\"", file=sys.stderr)
        print("(names are only known if some node has itself resolved that string "
              "-- see the wire-id-vs-name explanation in topic_directory.hpp)", file=sys.stderr)
        sys.exit(1)

    for tid, name in matches:
        print(f"Topic id : {tid}")
        print(f"Name     : {name if name else '(unnamed -- numeric topic id only)'}")
        print(f"Source   : {'rlcore ' + args.rlcore_ip if args.rlcore_ip else 'multicast broadcast'}")


def build_parser():
    # Shared options, added to BOTH the top-level parser (so they work
    # before the subcommand, e.g. "rl_topic.py --rlcore-ip x list") and
    # each subparser (so they also work after it, e.g.
    # "rl_topic.py list --rlcore-ip x") -- argparse subparsers don't
    # inherit a parent's options by position, only via `parents=`.
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--rlcore-ip", default=None,
                         help="Query rlcore directly instead of broadcasting over multicast.")
    common.add_argument("--rlcore-port", type=int, default=DEFAULT_RLCORE_PORT)
    common.add_argument("--group", default=DEFAULT_MULTICAST_GROUP,
                         help="Multicast group to query when --rlcore-ip is not given.")
    common.add_argument("--port", type=int, default=DEFAULT_MULTICAST_PORT)
    common.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT_S,
                         help="How long to wait for replies (seconds).")
    common.add_argument("--no-cache", action="store_true",
                         help="Don't read or write ~/.cache/relink/topic_names.json.")
    common.add_argument("--refresh", action="store_true",
                         help="Discard the existing cache before merging in fresh results.")

    p = argparse.ArgumentParser(
        prog="rl_topic.py",
        description="rostopic-style topic name directory CLI for ReLink.",
        parents=[common])

    sub = p.add_subparsers(dest="command", required=True)

    p_list = sub.add_parser("list", help="List every known topic id and name.", parents=[common])
    p_list.set_defaults(func=cmd_list)

    p_info = sub.add_parser("info", help="Show details for one topic (by name or numeric id).",
                             parents=[common])
    p_info.add_argument("topic", help="Topic name (e.g. /relink/imu) or numeric wire id.")
    p_info.set_defaults(func=cmd_info)

    p_hz = sub.add_parser("hz", help="Measure the publish rate of a topic (raw, type-agnostic).",
                           parents=[common])
    p_hz.add_argument("topic", help="Topic name or numeric wire id to subscribe to.")
    p_hz.add_argument("--window", type=int, default=100, help="Rolling sample window size.")
    p_hz.add_argument("--report-every", type=float, default=5.0,
                       help="Seconds of recent history to report on each tick.")
    p_hz.set_defaults(func=cmd_hz)

    p_bw = sub.add_parser("bw", help="Measure the bandwidth of a topic (raw, type-agnostic).",
                           parents=[common])
    p_bw.add_argument("topic", help="Topic name or numeric wire id to subscribe to.")
    p_bw.add_argument("--report-every", type=float, default=5.0,
                       help="Seconds of recent history to report on each tick.")
    p_bw.set_defaults(func=cmd_bw)

    p_echo = sub.add_parser("echo", help="Print messages on a topic as raw hex (type-agnostic).",
                             parents=[common])
    p_echo.add_argument("topic", help="Topic name or numeric wire id to subscribe to.")
    p_echo.add_argument("-n", "--count", type=int, default=0,
                         help="Stop after this many messages (default: run until Ctrl-C).")
    p_echo.set_defaults(func=cmd_echo)

    p_pub = sub.add_parser("pub", help="Publish a raw payload to a topic (type-agnostic).",
                            parents=[common])
    p_pub.add_argument("topic", help="Topic name or numeric wire id to publish to.")
    p_pub.add_argument("--hex", default=None, help="Payload as hex bytes, e.g. deadbeef.")
    p_pub.add_argument("--text", default=None, help="Payload as a UTF-8 string.")
    p_pub.add_argument("-r", "--repeat", type=int, default=1, help="Number of times to publish.")
    p_pub.add_argument("--rate-period", type=float, default=1.0,
                        help="Seconds between repeats when --repeat > 1.")
    p_pub.set_defaults(func=cmd_pub)

    return p


def main():
    args = build_parser().parse_args()
    args.func(args)


if __name__ == "__main__":
    main()

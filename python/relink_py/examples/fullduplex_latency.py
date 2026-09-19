#!/usr/bin/env python3
"""Full-duplex ReLink stress test -- two nodes, each simultaneously
publishing a steady stream AND subscribing to the other's stream, so
both directions carry real traffic concurrently (not ping/echo one at a
time). Same-machine test -- both processes share the same system clock,
so one-way latency via time.time() is valid here (no cross-machine skew
correction needed, unlike a real cross-network test).

Usage:
  python3 fullduplex_latency.py a <rlcore_ip> <rate_hz> <duration_sec>
  python3 fullduplex_latency.py b <rlcore_ip> <rate_hz> <duration_sec>

Run both roles (in either order, either terminal first -- rlcore-mode
discovery now re-registers periodically, so there's no ordering
requirement). Needs a relink-rlcore/relink_rlcore.py instance reachable
at <rlcore_ip>.
"""
import ctypes
import sys
import os
import time
import threading

# CPython's GIL only hands off between threads at most once per switch
# interval (default 5ms). This script's send loop is nearly always
# runnable at multi-kHz rates, so with the default interval it can hog
# the GIL long enough that the recv/dispatch thread falls behind
# draining the socket -- the kernel then drops packets before Python
# ever sees them. Shortening the interval makes the GIL hand off far
# more often, so the recv thread gets a fair share of CPU time even
# while the send loop is busy. Pure Python-runtime tuning -- has no
# equivalent need in the C++ binding, which has no GIL.
sys.setswitchinterval(0.0005)

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from relink import RelinkNode

# Real string topic names, not raw numeric ids -- advertise()/subscribe()
# hash these to a wire topic_id (via node._topic_id_for()) and, on
# rlcore-mode registration, RLNM-announce the name to rlcore, so
# rl_topic.py list/info can actually resolve them. A raw int topic (the
# old TOPIC_A2B = 900 here) skips that path entirely -- see
# RelinkNode._topic_id_for() -- so it never appears in rl_topic's
# directory even while data is flowing.
TOPIC_A2B = "/relink/fullduplex_a2b"
TOPIC_B2A = "/relink/fullduplex_b2a"


class Probe(ctypes.Structure):
    _pack_ = 1
    _fields_ = [("seq", ctypes.c_uint32), ("send_us", ctypes.c_uint64)]


def now_us():
    return int(time.time() * 1_000_000)


def main():
    if len(sys.argv) < 5:
        print(f"usage: {sys.argv[0]} a|b <rlcore_ip> <rate_hz> <duration_sec>", file=sys.stderr)
        return 1
    role = sys.argv[1]
    rlcore_ip = sys.argv[2]
    rate_hz = float(sys.argv[3])
    duration = float(sys.argv[4])

    out_topic = TOPIC_A2B if role == "a" else TOPIC_B2A
    in_topic = TOPIC_B2A if role == "a" else TOPIC_A2B

    node = RelinkNode()
    node.set_rlcore.ip(rlcore_ip)
    # Give the outbound and inbound topics separate sockets (ROS-style
    # per-topic ports) instead of sharing one. With the default shared
    # socket, this node's own busy send loop and ReLink's recv/dispatch
    # thread -- plus every rlcore registration/re-registration ACK --
    # all contend for the SAME file descriptor, which is exactly what
    # caused the registration-ACK race (data thread "stealing" the ACK)
    # diagnosed earlier. Splitting them removes that contention entirely:
    # sending never blocks/competes with receiving, and vice versa.
    node.set_multiplex(False)

    latencies_ms = []
    received = 0
    max_seq_seen = -1
    lock = threading.Lock()

    def on_msg(m: Probe):
        nonlocal received, max_seq_seen
        lat_ms = (now_us() - m.send_us) / 1000.0
        with lock:
            received += 1
            latencies_ms.append(lat_ms)
            if m.seq > max_seq_seen:
                max_seq_seen = m.seq

    node.advertise(out_topic, Probe)
    node.subscribe(in_topic, Probe, on_msg)
    # _topic_transports is keyed by the resolved numeric topic_id, not
    # the name -- advertise()/subscribe() above already resolved+cached
    # it via _topic_id_for(), so this just looks it up again.
    out_topic_id = node._topic_id_for(out_topic)
    in_topic_id = node._topic_id_for(in_topic)
    # Same buffer-headroom reasoning as before, applied per-topic now
    # that each has its own dedicated socket.
    node._topic_transports[out_topic_id].enable_large_buffers()
    node._topic_transports[in_topic_id].enable_large_buffers()

    print(f"[{role}] full-duplex stress: out={out_topic} (id {out_topic_id}) "
          f"in={in_topic} (id {in_topic_id}) "
          f"rate={rate_hz}Hz duration={duration}s rlcore={rlcore_ip}")

    period = 1.0 / rate_hz
    # No artificial sleep floor -- at high rates (multi-kHz) a fixed
    # minimum sleep (e.g. 0.5ms) silently caps achievable throughput
    # well below the requested rate. Busy-poll with spin_once() and only
    # sleep the leftover time until the next send is actually due,
    # clamped at 0 (never a negative sleep).
    start = time.time()
    seq = 0
    sent = 0
    next_send = start
    while time.time() - start < duration:
        now = time.time()
        if now >= next_send:
            msg = Probe(seq=seq, send_us=now_us())
            node.publish(out_topic, msg)
            seq += 1
            sent += 1
            next_send += period
        node.spin_once()
        remaining = next_send - time.time()
        if remaining > 0:
            time.sleep(min(remaining, 0.001))

    # Drain a little longer to catch in-flight messages after the send
    # loop stops, so trailing latency samples aren't lost.
    drain_until = time.time() + 0.5
    while time.time() < drain_until:
        node.spin_once()
        time.sleep(0.005)

    with lock:
        lats = sorted(latencies_ms)
        n = len(lats)

    def pct(p):
        if not lats:
            return -1.0
        idx = int(p * (len(lats) - 1))
        return lats[idx]

    actual_send_hz = sent / duration
    print(f"[{role}] sent={sent} (actual {actual_send_hz:.0f}Hz) received={received} "
          f"max_peer_seq_seen={max_seq_seen}")
    if lats:
        print(f"[{role}] latency_ms: p50={pct(0.5):.2f} p90={pct(0.9):.2f} "
              f"p99={pct(0.99):.2f} min={lats[0]:.2f} max={lats[-1]:.2f} n={n}")
    else:
        print(f"[{role}] latency: no samples received (peer not up / no traffic incoming)")

    return 0


if __name__ == "__main__":
    sys.exit(main())

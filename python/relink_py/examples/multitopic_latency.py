#!/usr/bin/env python3
"""Multi-topic full-duplex ReLink stress test -- two nodes, each running
N independent topic PAIRS (2N topics total) concurrently, every pair its
own dedicated socket (set_multiplex(False)), each publishing AND
subscribing at the same time. Extends fullduplex_latency.py from one
pair to many, to stress registration/re-registration, NAT-punch bursts,
and per-topic socket overhead at realistic topic counts (e.g. a real
robot with 20 sensor/control topics), not just one pub/sub pair.

Usage:
  python3 multitopic_latency.py a <rlcore_ip> <num_topic_pairs> <rate_hz_per_topic> <duration_sec>
  python3 multitopic_latency.py b <rlcore_ip> <num_topic_pairs> <rate_hz_per_topic> <duration_sec>

Run both roles (in either order, either terminal first -- no ordering
requirement). Needs a relink-rlcore/relink_rlcore.py instance reachable
at <rlcore_ip>.
"""
import ctypes
import sys
import os
import time
import threading

sys.setswitchinterval(0.0005)  # see fullduplex_latency.py for rationale

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from relink import RelinkNode

BASE_A2B = 1000  # topics 1000..1000+N-1 are A->B
BASE_B2A = 2000  # topics 2000..2000+N-1 are B->A


class Probe(ctypes.Structure):
    _pack_ = 1
    _fields_ = [("seq", ctypes.c_uint32), ("send_us", ctypes.c_uint64)]


def now_us():
    return int(time.time() * 1_000_000)


def main():
    if len(sys.argv) < 6:
        print(f"usage: {sys.argv[0]} a|b <rlcore_ip> <num_topic_pairs> <rate_hz_per_topic> <duration_sec>",
              file=sys.stderr)
        return 1
    role = sys.argv[1]
    rlcore_ip = sys.argv[2]
    n_pairs = int(sys.argv[3])
    rate_hz = float(sys.argv[4])
    duration = float(sys.argv[5])

    out_topics = [BASE_A2B + i for i in range(n_pairs)] if role == "a" else [BASE_B2A + i for i in range(n_pairs)]
    in_topics = [BASE_B2A + i for i in range(n_pairs)] if role == "a" else [BASE_A2B + i for i in range(n_pairs)]

    node = RelinkNode()
    node.set_rlcore.ip(rlcore_ip)
    node.set_multiplex(False)  # dedicated socket per topic -- see fullduplex_latency.py

    lock = threading.Lock()
    per_topic_recv = {t: 0 for t in in_topics}
    per_topic_lat = {t: [] for t in in_topics}
    per_topic_max_seq = {t: -1 for t in in_topics}

    def make_handler(topic):
        def on_msg(m: Probe):
            lat_ms = (now_us() - m.send_us) / 1000.0
            with lock:
                per_topic_recv[topic] += 1
                per_topic_lat[topic].append(lat_ms)
                if m.seq > per_topic_max_seq[topic]:
                    per_topic_max_seq[topic] = m.seq
        return on_msg

    for t in out_topics:
        node.advertise(t, Probe)
    for t in in_topics:
        node.subscribe(t, Probe, make_handler(t))
    for t in out_topics + in_topics:
        node._topic_transports[t].enable_large_buffers()

    print(f"[{role}] multi-topic stress: {n_pairs} topic pairs ({len(out_topics)} out + {len(in_topics)} in = "
          f"{len(out_topics) + len(in_topics)} sockets), rate={rate_hz}Hz/topic "
          f"(aggregate {rate_hz * n_pairs:.0f}Hz), duration={duration}s, rlcore={rlcore_ip}")

    period = 1.0 / rate_hz
    start = time.time()
    seqs = {t: 0 for t in out_topics}
    sent = {t: 0 for t in out_topics}
    next_send = {t: start + i * (period / max(1, n_pairs)) for i, t in enumerate(out_topics)}  # phase-stagger topics

    while time.time() - start < duration:
        now = time.time()
        soonest = None
        for t in out_topics:
            if now >= next_send[t]:
                msg = Probe(seq=seqs[t], send_us=now_us())
                node.publish(t, msg)
                seqs[t] += 1
                sent[t] += 1
                next_send[t] += period
            if soonest is None or next_send[t] < soonest:
                soonest = next_send[t]
        node.spin_once()
        remaining = (soonest - time.time()) if soonest else period
        if remaining > 0:
            time.sleep(min(remaining, 0.001))

    drain_until = time.time() + 0.5
    while time.time() < drain_until:
        node.spin_once()
        time.sleep(0.005)

    total_sent = sum(sent.values())
    total_recv = sum(per_topic_recv.values())
    with lock:
        all_lats = sorted(l for lats in per_topic_lat.values() for l in lats)

    def pct(lats, p):
        if not lats:
            return -1.0
        idx = int(p * (len(lats) - 1))
        return lats[idx]

    # Own-sent isn't the right denominator for "how much of what I
    # received actually arrived" -- that's inherently about the PEER's
    # send volume, which this process can only estimate from the
    # highest sequence number it's seen per topic (peer_max_seq+1). Not
    # exact (a few of the peer's tail messages may still be in flight),
    # but far closer than dividing by this node's own unrelated send
    # count, which was silently wrong in an earlier version of this
    # script.
    est_peer_sent = sum(per_topic_max_seq[t] + 1 for t in in_topics if per_topic_max_seq[t] >= 0)
    print(f"[{role}] TOTAL sent={total_sent} received={total_recv} "
          f"est_peer_sent={est_peer_sent} "
          f"delivery={100.0*total_recv/max(1,est_peer_sent):.2f}%")
    if all_lats:
        print(f"[{role}] TOTAL latency_ms: p50={pct(all_lats,0.5):.2f} p90={pct(all_lats,0.9):.2f} "
              f"p99={pct(all_lats,0.99):.2f} max={all_lats[-1]:.2f} n={len(all_lats)}")

    print(f"[{role}] per-topic received counts: " +
          ", ".join(f"{t}:{per_topic_recv[t]}" for t in in_topics))

    return 0


if __name__ == "__main__":
    sys.exit(main())

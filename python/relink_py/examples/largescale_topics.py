#!/usr/bin/env python3
"""Large-topic-count ReLink stress test -- simulates a real large system
(e.g. a robot fleet with ~1000 distinct sensor/control topics) running on
ONE shared multiplexed socket (default multiplex mode, NOT
set_multiplex(False)): with N topic pairs that's 2N real string-named
topics, every one of them actually advertised/subscribed and carrying
real periodic values, all through a single UdpTransport/socket/recv
thread -- exactly the case the registration-ACK race fix
(register_with_rlcore_on_socket() handing RegisterAck packets off through
a queue instead of racing the data thread for recvfrom()) was meant to
make safe at scale. Each topic gets a REAL name (e.g.
"/relink/stress/sensor_0417") so relink_rlcore's name directory actually
learns it and rl_topic.py list/info can be used against a live run to
prove name resolution holds up at this topic count, not just at a
handful.

Usage:
  python3 largescale_topics.py a <rlcore_ip> <num_topic_pairs> <rate_hz_per_topic> <duration_sec>
  python3 largescale_topics.py b <rlcore_ip> <num_topic_pairs> <rate_hz_per_topic> <duration_sec>

Example (~1000 total topics, 500 pairs, 2Hz each, 30s):
  python3 largescale_topics.py a 127.0.0.1 500 2 30
  python3 largescale_topics.py b 127.0.0.1 500 2 30

While both are running, from another terminal:
  python3 rl_topic.py list --rlcore-ip 127.0.0.1 | wc -l     # should be ~2x num_topic_pairs
  python3 rl_topic.py info /relink/stress/a2b_0000 --rlcore-ip 127.0.0.1
"""
import ctypes
import sys
import os
import time
import threading

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from relink import RelinkNode


class Probe(ctypes.Structure):
    _pack_ = 1
    _fields_ = [("seq", ctypes.c_uint32), ("send_us", ctypes.c_uint64), ("value", ctypes.c_double)]


def now_us():
    return int(time.time() * 1_000_000)


def topic_name(direction: str, i: int, n_pairs: int) -> str:
    width = len(str(n_pairs - 1))
    return f"/relink/stress/{direction}_{i:0{width}d}"


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

    out_dir = "a2b" if role == "a" else "b2a"
    in_dir = "b2a" if role == "a" else "a2b"
    out_topics = [topic_name(out_dir, i, n_pairs) for i in range(n_pairs)]
    in_topics = [topic_name(in_dir, i, n_pairs) for i in range(n_pairs)]

    node = RelinkNode()
    node.set_rlcore.ip(rlcore_ip)
    # Deliberately DEFAULT (shared) multiplex here, not set_multiplex(False):
    # 2*n_pairs dedicated sockets/threads (one recv thread each) is the
    # wrong shape for a real ~1000-topic system and was never the point
    # of set_multiplex(False) -- that flag exists for a handful of
    # bandwidth-heavy topics (e.g. Image), not thousands of small ones.
    # One shared socket is exactly what previously raced the
    # registration ACK against the data thread; that race is now closed
    # (see rlcore_client.py's `transport` param), so this is the real
    # test of whether that fix holds at realistic topic counts.
    node._transport.enable_large_buffers()

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

    t_declare_start = time.time()
    for t in out_topics:
        node.advertise(t, Probe)
    for t in in_topics:
        node.subscribe(t, Probe, make_handler(t))
    t_declare = time.time() - t_declare_start

    print(f"[{role}] large-scale stress: {n_pairs} topic pairs "
          f"({len(out_topics)} out + {len(in_topics)} in = {len(out_topics) + len(in_topics)} "
          f"real named topics, ONE shared socket), rate={rate_hz}Hz/topic "
          f"(aggregate {rate_hz * n_pairs:.0f}Hz), duration={duration}s, rlcore={rlcore_ip}, "
          f"declare_time={t_declare:.2f}s")

    period = 1.0 / rate_hz
    start = time.time()
    seqs = {t: 0 for t in out_topics}
    sent = {t: 0 for t in out_topics}
    # Phase-stagger so all N topics don't all try to send on the exact
    # same tick -- spreads the aggregate send load evenly across the
    # period instead of bursting.
    next_send = {t: start + i * (period / max(1, n_pairs)) for i, t in enumerate(out_topics)}

    while time.time() - start < duration:
        now = time.time()
        soonest = None
        for t in out_topics:
            if now >= next_send[t]:
                msg = Probe(seq=seqs[t], send_us=now_us(), value=seqs[t] * 0.1)
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

    drain_until = time.time() + 1.0
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

    est_peer_sent = sum(per_topic_max_seq[t] + 1 for t in in_topics if per_topic_max_seq[t] >= 0)
    topics_with_zero_recv = sum(1 for t in in_topics if per_topic_recv[t] == 0)
    print(f"[{role}] TOTAL sent={total_sent} received={total_recv} "
          f"est_peer_sent={est_peer_sent} "
          f"delivery={100.0*total_recv/max(1,est_peer_sent):.2f}% "
          f"topics_with_zero_received={topics_with_zero_recv}/{len(in_topics)}")
    if all_lats:
        print(f"[{role}] TOTAL latency_ms: p50={pct(all_lats,0.5):.2f} p90={pct(all_lats,0.9):.2f} "
              f"p99={pct(all_lats,0.99):.2f} max={all_lats[-1]:.2f} n={len(all_lats)}")

    return 0


if __name__ == "__main__":
    sys.exit(main())

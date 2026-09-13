#!/usr/bin/env python3
"""relink-rlcore (Python build) -- small standalone registration daemon,
per relink-com-spec.md "Mode A: relink-rlcore" section.

Must produce byte-identical RegisterRequest/RegisterAck wire packets to
the C++ build (rlcore/relink_rlcore.cpp) -- same struct layout,
little-endian, no framework beyond the standard library (socket + struct),
per the spec's "no heavy serialization" rule applied to every language.

Wire layout (matches relink/include/relink/register.hpp exactly, topic_id
widened to uint32_t -- see relink_py/relink/register.py, the reference
this file was previously out of sync with):
    RegisterRequestHeader = "<IHH"  node_ip(u32) node_port(u16) topic_count(u16)
                            + topic_count * "<I"
    RegisterAckHeader     = "<BH"   status(u8) peer_count(u16)
                            + peer_count * RegisterAckPeer("<IHI" ip,port,topic_id)

--nat: NAT traversal / UDP hole punching support, mirrors
rlcore/relink_rlcore.cpp's --nat flag exactly (byte-identical
behavior, same rationale). When set, the OBSERVED UDP source address of
each registration (the real, NAT-mapped endpoint) is used instead of the
self-reported node_ip/node_port in the payload, which is typically a
private LAN address useless to a peer on a different network. Opening
the actual NAT hole additionally requires each RelinkNode client to send
a punch-packet burst to every peer it learns about -- see
relink/node.py's _ensure_started().
"""
import os
import socket
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "relink_py"))
from relink import topic_directory as tdir

DEFAULT_PORT = 8445

REQ_HEADER_FMT = "<IHH"     # node_ip, node_port, topic_count
REQ_HEADER_LEN = struct.calcsize(REQ_HEADER_FMT)
ACK_HEADER_FMT = "<BH"      # status, peer_count
ACK_HEADER_LEN = struct.calcsize(ACK_HEADER_FMT)
PEER_FMT = "<IHI"           # ip, port, topic_id
PEER_LEN = struct.calcsize(PEER_FMT)


def decode_register_request(buf: bytes):
    if len(buf) < REQ_HEADER_LEN:
        return None
    node_ip, node_port, topic_count = struct.unpack_from(REQ_HEADER_FMT, buf, 0)
    expected = REQ_HEADER_LEN + topic_count * 4
    if len(buf) != expected:
        return None
    topics = list(struct.unpack_from("<%dI" % topic_count, buf, REQ_HEADER_LEN)) if topic_count else []
    return node_ip, node_port, topics


def encode_register_ack(status: int, peers) -> bytes:
    out = bytearray(struct.pack(ACK_HEADER_FMT, status, len(peers)))
    for ip, port, topic_id in peers:
        out += struct.pack(PEER_FMT, ip, port, topic_id)
    return bytes(out)


def main():
    port = DEFAULT_PORT
    if "--port" in sys.argv:
        port = int(sys.argv[sys.argv.index("--port") + 1])
    nat_mode = "--nat" in sys.argv

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", port))
    suffix = " (NAT traversal enabled)" if nat_mode else ""
    print(f"relink-rlcore (Python) listening on 0.0.0.0:{port}{suffix}", flush=True)

    table = {}  # topic_id -> set of (ip, port)

    # topic_id -> name, accumulated from RLNM announces sent by any node
    # registered here (see relink_py/relink/topic_directory.py) -- lets
    # rl_topic.py ask rlcore for every topic name any node in this fleet
    # has resolved, via a single RLNQ query, instead of reaching each
    # node individually.
    topic_names = {}

    while True:
        data, addr = sock.recvfrom(max(2048, tdir.MAX_PACKET))

        kind = tdir.packet_kind(data)
        if kind == "announce":
            entries = tdir.decode_entries(data)
            if entries is not None:
                for e in entries:
                    topic_names[e.topic_id] = e.name
            continue
        if kind == "query":
            entries = [tdir.TopicDirEntry(tid, name) for tid, name in topic_names.items()]
            try:
                sock.sendto(tdir.encode_reply(entries), addr)
            except (ValueError, OSError):
                pass
            continue
        if kind == "reply":
            continue  # rlcore never queries anyone itself

        decoded = decode_register_request(data)
        if decoded is None:
            print("relink-rlcore: dropped malformed RegisterRequest", file=sys.stderr, flush=True)
            continue
        node_ip, node_port, topics = decoded

        if nat_mode:
            # addr is (ip_str, port) as observed by the OS -- the real,
            # NAT-mapped endpoint, not whatever private address the node
            # self-reported in the payload. inet_aton returns network-
            # order (big-endian) bytes, so unpack with ">I" -- "<I" here
            # would silently byte-reverse the address (127.0.0.1 becomes
            # 1.0.0.127), the same host-order convention used everywhere
            # else in this codebase (see relink/register.hpp's
            # ipv4_to_host_order / the ">I" pack just below for the
            # reverse direction).
            observed_ip = struct.unpack(">I", socket.inet_aton(addr[0]))[0]
            self_entry = (observed_ip, addr[1])
        else:
            self_entry = (node_ip, node_port)

        for topic in topics:
            table.setdefault(topic, set()).add(self_entry)

        peers = []
        for topic in topics:
            for (ip, p) in table.get(topic, ()):
                if (ip, p) == self_entry:
                    continue
                peers.append((ip, p, topic))

        ack = encode_register_ack(0, peers)
        sock.sendto(ack, addr)

        # self_entry's ip is stored host-byte-order (see
        # relink/register.hpp / ipv4_to_host_order) -- pack big-endian to
        # get network order for inet_ntoa, regardless of this machine's
        # own endianness.
        ip_str = socket.inet_ntoa(struct.pack(">I", self_entry[0]))
        observed_suffix = " [observed]" if nat_mode else ""
        print(f"relink-rlcore: registered {ip_str}:{self_entry[1]} "
              f"({len(topics)} topics){observed_suffix}, replied with {len(peers)} peers", flush=True)


if __name__ == "__main__":
    main()

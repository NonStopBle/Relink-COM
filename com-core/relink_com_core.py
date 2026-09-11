#!/usr/bin/env python3
"""relink-com-core (Python build) -- small standalone registration daemon,
per relink-com-spec.md "Mode A: relink-com-core" section.

Must produce byte-identical RegisterRequest/RegisterAck wire packets to
the C++ build (com-core/relink_com_core.cpp) -- same struct layout,
little-endian, no framework beyond the standard library (socket + struct),
per the spec's "no heavy serialization" rule applied to every language.

Wire layout (matches relink/include/relink/register.hpp exactly):
    RegisterRequestHeader = "<IHH"  node_ip(u32) node_port(u16) topic_count(u16)
                            + topic_count * "<H"
    RegisterAckHeader     = "<BH"   status(u8) peer_count(u16)
                            + peer_count * RegisterAckPeer("<IHH" ip,port,topic_id)
"""
import socket
import struct
import sys

DEFAULT_PORT = 8445

REQ_HEADER_FMT = "<IHH"     # node_ip, node_port, topic_count
REQ_HEADER_LEN = struct.calcsize(REQ_HEADER_FMT)
ACK_HEADER_FMT = "<BH"      # status, peer_count
ACK_HEADER_LEN = struct.calcsize(ACK_HEADER_FMT)
PEER_FMT = "<IHH"           # ip, port, topic_id
PEER_LEN = struct.calcsize(PEER_FMT)


def decode_register_request(buf: bytes):
    if len(buf) < REQ_HEADER_LEN:
        return None
    node_ip, node_port, topic_count = struct.unpack_from(REQ_HEADER_FMT, buf, 0)
    expected = REQ_HEADER_LEN + topic_count * 2
    if len(buf) != expected:
        return None
    topics = list(struct.unpack_from("<%dH" % topic_count, buf, REQ_HEADER_LEN)) if topic_count else []
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

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", port))
    print(f"relink-com-core (Python) listening on 0.0.0.0:{port}", flush=True)

    table = {}  # topic_id -> set of (ip, port)

    while True:
        data, addr = sock.recvfrom(2048)
        decoded = decode_register_request(data)
        if decoded is None:
            print("relink-com-core: dropped malformed RegisterRequest", file=sys.stderr, flush=True)
            continue
        node_ip, node_port, topics = decoded
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

        # node_ip is stored host-byte-order (see relink/register.hpp /
        # ipv4_to_host_order) -- pack big-endian to get network order for
        # inet_ntoa, regardless of this machine's own endianness.
        ip_str = socket.inet_ntoa(struct.pack(">I", node_ip))
        print(f"relink-com-core: registered {ip_str}:{node_port} "
              f"({len(topics)} topics), replied with {len(peers)} peers", flush=True)


if __name__ == "__main__":
    main()

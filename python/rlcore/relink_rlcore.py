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
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "relink_py"))
from relink import topic_directory as tdir
from relink.crypto import (
    generate_random_key32, key32_to_hex, hex_to_key32, aes256gcm_seal, aes256gcm_open,
)

DEFAULT_PORT = 8445

# A live node re-registers every 0.3s for as long as it's running (see
# node.py's _rlcore_reregister_interval) -- a registration this stale
# means the process exited (or died) without rlcore ever finding out,
# since there's no unregister-on-close message on this wire protocol.
# ~10x the reregister interval gives plenty of margin for a slow/missed
# tick without treating a genuinely dead node as still alive for long.
REGISTRATION_TTL_S = 3.0

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
    if "--generate-key" in sys.argv:
        # Prints a fresh random AES-256 key and exits -- does not start
        # the daemon. Run once, then pass the printed hex to both this
        # daemon's --encrypt-key and every node's
        # node.set_rlcore.set_encrypt_key(...); the same key must be
        # used on both sides for registration to succeed.
        print(key32_to_hex(generate_random_key32()))
        return

    port = DEFAULT_PORT
    if "--port" in sys.argv:
        port = int(sys.argv[sys.argv.index("--port") + 1])
    nat_mode = "--nat" in sys.argv

    encrypt_key = None
    if "--encrypt-key" in sys.argv:
        encrypt_key = hex_to_key32(sys.argv[sys.argv.index("--encrypt-key") + 1])
        if encrypt_key is None:
            print("relink-rlcore: --encrypt-key expects 64 hex characters "
                  "(a 32-byte AES-256 key) -- generate one with --generate-key",
                  file=sys.stderr)
            sys.exit(1)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", port))
    suffix = " (NAT traversal enabled)" if nat_mode else ""
    print(f"relink-rlcore (Python) listening on 0.0.0.0:{port}{suffix}", flush=True)

    table = {}  # topic_id -> set of (ip, port)

    # (topic_id, ip, port) -> time.time() of its most recent
    # RegisterRequest -- lets prune_stale() below evict a registration
    # once its owning node stops re-registering (closed/crashed), so
    # `rl_topic.py list`/`info` and peer discovery both stop treating a
    # dead node's topics as live. See REGISTRATION_TTL_S.
    last_seen = {}

    # topic_id -> name, accumulated from RLNM announces sent by any node
    # registered here (see relink_py/relink/topic_directory.py) -- lets
    # rl_topic.py ask rlcore for every topic name any node in this fleet
    # has resolved, via a single RLNQ query, instead of reaching each
    # node individually.
    topic_names = {}

    # (topic_id, ip, port) -> role bitmask (tdir.ROLE_PUBLISHER |
    # tdir.ROLE_SUBSCRIBER), from periodic RLPA role announces -- lets
    # `rl_topic.py info` show who's publishing vs subscribing a topic
    # (see topic_directory.py's role-directory section for the wire
    # protocol). Pruned by the same prune_stale() / TTL as registrations,
    # via its own last-seen map below, since role announces are sent on
    # the same periodic cadence as reregistration.
    role_table = {}
    role_last_seen = {}

    def prune_stale():
        now = time.time()
        for topic in list(table.keys()):
            live = {(ip, p) for (ip, p) in table[topic]
                    if now - last_seen.get((topic, ip, p), 0.0) <= REGISTRATION_TTL_S}
            for (ip, p) in table[topic] - live:
                last_seen.pop((topic, ip, p), None)
            if live:
                table[topic] = live
            else:
                del table[topic]
        for key in [k for k, ts in role_last_seen.items() if now - ts > REGISTRATION_TTL_S]:
            role_last_seen.pop(key, None)
            role_table.pop(key, None)

    # Wake up periodically even with no incoming traffic, purely to run
    # prune_stale() -- otherwise a fleet that goes quiet keeps every last
    # registration "alive" forever, since nothing else ever calls it.
    sock.settimeout(1.0)

    while True:
        # 65507 (largest possible UDP/IPv4 datagram), not
        # max(2048, tdir.MAX_PACKET) (8192): a large RegisterRequest
        # (many topics) or an RLNM announce chunk (up to
        # tdir.MAX_ENTRIES=512 names) can legitimately exceed 8192 bytes
        # -- e.g. 500 real topic names measured at 13506 bytes here --
        # and recvfrom()'s length argument silently truncates anything
        # past it at the kernel level for UDP, with no error raised.
        # That corrupted the packet into something decode_entries()/
        # decode_register_request() correctly rejects as malformed, so
        # a real large-topic-count node's announce was dropped with no
        # error printed anywhere, even though it was sent successfully.
        try:
            data, addr = sock.recvfrom(65507)
        except socket.timeout:
            prune_stale()
            continue
        prune_stale()

        kind = tdir.packet_kind(data)
        if kind == "announce":
            entries = tdir.decode_entries(data)
            if entries is not None:
                for e in entries:
                    topic_names[e.topic_id] = e.name
            continue
        if kind == "query":
            # tdir.chunk_entries(), not a raw MAX_ENTRIES slice: at real
            # large-topic-count scale (e.g. ~1000 topics across a fleet)
            # topic_names can need multiple RLNR reply packets, and
            # MAX_ENTRIES alone doesn't guarantee each chunk's encoded
            # size stays under MAX_PACKET once names have real-world
            # length -- see chunk_entries()'s docstring. rl_topic.py's
            # query_rlcore() collects every chunk sent here.
            #
            # Include every topic id `table` currently has a LIVE
            # registration for (prune_stale() already dropped anything
            # past REGISTRATION_TTL_S), not just ones that got a name via
            # RLNM -- a topic advertised/subscribed with a raw numeric id
            # (no string name) is real and actively routed, but would
            # otherwise be completely invisible to `rl_topic.py list`.
            # Such ids are sent with an empty name (wire format already
            # supports name_len=0); topic_names still wins for anything
            # named. Deliberately NOT unioned with topic_names' own keys:
            # a name whose topic has no live registrant left (the node
            # that announced it exited) must stop being listed too --
            # that's the whole point of this bug fix.
            entries = [tdir.TopicDirEntry(tid, topic_names.get(tid, "")) for tid in table.keys()]
            for chunk in (tdir.chunk_entries(entries) or [[]]):
                try:
                    sock.sendto(tdir.encode_reply(chunk), addr)
                except (ValueError, OSError):
                    pass
            continue
        if kind == "reply":
            continue  # rlcore never queries anyone itself

        role_kind = tdir.role_packet_kind(data)
        if role_kind == "role_announce":
            # Always the OBSERVED UDP source, regardless of --nat -- this
            # is purely diagnostic ("who is really talking to me on this
            # topic"), and the observed source is always the true
            # endpoint for that purpose, unlike node_ip/node_port in a
            # RegisterRequest payload which self-reports a possibly
            # private/unroutable address.
            entries = tdir.decode_role_announce(data)
            if entries is not None:
                observed_ip = struct.unpack(">I", socket.inet_aton(addr[0]))[0]
                now = time.time()
                for e in entries:
                    key = (e.topic_id, observed_ip, addr[1])
                    role_table[key] = e.role
                    role_last_seen[key] = now
            continue
        if role_kind == "role_query":
            entries = [tdir.RolePeerEntry(tid, role, ip, port)
                       for (tid, ip, port), role in role_table.items()]
            for chunk in (tdir.chunk_role_peer_entries(entries) or [[]]):
                try:
                    sock.sendto(tdir.encode_role_reply(chunk), addr)
                except (ValueError, OSError):
                    pass
            continue
        if role_kind == "role_reply":
            continue  # rlcore never queries anyone itself

        # When --encrypt-key is set, every RegisterRequest must be an
        # AES-256-GCM-sealed blob under that key -- opened here before
        # decoding. A plaintext or wrong-key request fails to open and
        # is dropped the same way a malformed one always was; this also
        # authenticates the sender (GCM's tag), not just hides the payload.
        req_data = data
        if encrypt_key is not None:
            opened = aes256gcm_open(encrypt_key, data)
            if opened is None:
                print("relink-rlcore: dropped RegisterRequest that failed to decrypt "
                      "(missing/wrong key on the sending node?)", file=sys.stderr, flush=True)
                continue
            req_data = opened

        decoded = decode_register_request(req_data)
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

        now = time.time()
        for topic in topics:
            table.setdefault(topic, set()).add(self_entry)
            last_seen[(topic,) + self_entry] = now

        peers = []
        for topic in topics:
            for (ip, p) in table.get(topic, ()):
                if (ip, p) == self_entry:
                    continue
                peers.append((ip, p, topic))

        ack = encode_register_ack(0, peers)
        if encrypt_key is not None:
            ack = aes256gcm_seal(encrypt_key, ack)
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

"""RegisterRequest / RegisterAck wire encode/decode -- mode A (rlcore)
discovery protocol. Mirrors relink/include/relink/register.hpp and must
stay byte-identical to rlcore/relink_rlcore.py's own encoding (that
file predates this package and already proved C++/Python interop at the
daemon level; this module is the equivalent codec for a full node-side
Python binding, kept in lockstep with it deliberately)."""

import struct
from typing import List, NamedTuple

RLCORE_DEFAULT_PORT = 8445

_REQ_HEADER_FMT = "<IHH"   # node_ip, node_port, topic_count
_REQ_HEADER_LEN = struct.calcsize(_REQ_HEADER_FMT)
_ACK_HEADER_FMT = "<BH"    # status, peer_count
_ACK_HEADER_LEN = struct.calcsize(_ACK_HEADER_FMT)
_PEER_FMT = "<IHIIH"       # ip, port, topic_id, lan_ip, lan_port
_PEER_LEN = struct.calcsize(_PEER_FMT)


def encode_register_request(node_ip: int, node_port: int, topic_ids: List[int]) -> bytes:
    out = struct.pack(_REQ_HEADER_FMT, node_ip, node_port, len(topic_ids))
    if topic_ids:
        out += struct.pack("<%dI" % len(topic_ids), *topic_ids)
    return out


class DecodedRegisterRequest(NamedTuple):
    node_ip: int
    node_port: int
    topic_ids: List[int]


def decode_register_request(buf: bytes) -> DecodedRegisterRequest:
    if len(buf) < _REQ_HEADER_LEN:
        raise ValueError("RegisterRequest too short")
    node_ip, node_port, topic_count = struct.unpack_from(_REQ_HEADER_FMT, buf, 0)
    expected = _REQ_HEADER_LEN + topic_count * 4
    if len(buf) != expected:
        raise ValueError("RegisterRequest length mismatch")
    topics = list(struct.unpack_from("<%dI" % topic_count, buf, _REQ_HEADER_LEN)) if topic_count else []
    return DecodedRegisterRequest(node_ip, node_port, topics)


class RegisterAckPeer(NamedTuple):
    ip: int
    port: int
    topic_id: int
    # Same-NAT ("hairpin") LAN fallback candidate -- see wire.py's
    # RegisterAckPeer doc comment. 0/0 means "none known".
    lan_ip: int = 0
    lan_port: int = 0


def encode_register_ack(status: int, peers: List[RegisterAckPeer]) -> bytes:
    out = struct.pack(_ACK_HEADER_FMT, status, len(peers))
    for p in peers:
        out += struct.pack(_PEER_FMT, p.ip, p.port, p.topic_id, p.lan_ip, p.lan_port)
    return out


class DecodedRegisterAck(NamedTuple):
    status: int
    peers: List[RegisterAckPeer]


def decode_register_ack(buf: bytes) -> DecodedRegisterAck:
    if len(buf) < _ACK_HEADER_LEN:
        raise ValueError("RegisterAck too short")
    status, peer_count = struct.unpack_from(_ACK_HEADER_FMT, buf, 0)
    expected = _ACK_HEADER_LEN + peer_count * _PEER_LEN
    if len(buf) != expected:
        raise ValueError("RegisterAck length mismatch")
    peers = []
    off = _ACK_HEADER_LEN
    for _ in range(peer_count):
        ip, port, topic_id, lan_ip, lan_port = struct.unpack_from(_PEER_FMT, buf, off)
        peers.append(RegisterAckPeer(ip, port, topic_id, lan_ip, lan_port))
        off += _PEER_LEN
    return DecodedRegisterAck(status, peers)

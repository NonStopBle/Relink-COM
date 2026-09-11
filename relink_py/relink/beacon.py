"""BeaconPacket wire encode/decode -- mode B (multicast) discovery
protocol, mirrors relink/include/relink/beacon.hpp."""

import struct
from typing import List, NamedTuple

_HEADER_FMT = "<IHH"  # node_ip, node_port, topic_count
_HEADER_LEN = struct.calcsize(_HEADER_FMT)


def encode_beacon_packet(node_ip: int, node_port: int, topic_ids: List[int]) -> bytes:
    out = struct.pack(_HEADER_FMT, node_ip, node_port, len(topic_ids))
    if topic_ids:
        out += struct.pack("<%dI" % len(topic_ids), *topic_ids)
    return out


class DecodedBeacon(NamedTuple):
    node_ip: int
    node_port: int
    topic_ids: List[int]


def decode_beacon_packet(buf: bytes) -> DecodedBeacon:
    if len(buf) < _HEADER_LEN:
        raise ValueError("BeaconPacket too short")
    node_ip, node_port, topic_count = struct.unpack_from(_HEADER_FMT, buf, 0)
    expected = _HEADER_LEN + topic_count * 4
    if len(buf) != expected:
        raise ValueError("BeaconPacket length mismatch")
    topics = list(struct.unpack_from("<%dI" % topic_count, buf, _HEADER_LEN)) if topic_count else []
    return DecodedBeacon(node_ip, node_port, topics)

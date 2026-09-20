"""BeaconPacket wire encode/decode -- mode B (multicast) discovery
protocol, mirrors relink/include/relink/beacon.hpp.

v1 layout: BeaconPacketHeader(8B: node_ip, node_port, topic_count) +
topic_count * uint32 topic_ids.

Phase 5 of the local-IPC plan adds an OPTIONAL trailing extension (8B:
node_pid, capability_flags) so a same-host shared-memory-capable peer
can be recognized -- see node.py's advertise_local_ipc/
subscribe_local_ipc. Appended after the topic list, and genuinely
optional on decode (length check is ">=", not "=="): a beacon without
the extension still decodes fine (pid/shm_capable default to 0/False),
and a beacon WITH it still decodes fine on a decoder that predates this
field, AS LONG AS that decoder was also written with ">=" -- the
original "==" check would have rejected any beacon carrying this
extension outright, so that check had to be relaxed here rather than
left as historical byte-count strictness."""

import struct
from typing import List, NamedTuple

_HEADER_FMT = "<IHH"  # node_ip, node_port, topic_count
_HEADER_LEN = struct.calcsize(_HEADER_FMT)

_EXT_FMT = "<II"  # node_pid, capability_flags
_EXT_LEN = struct.calcsize(_EXT_FMT)

CAP_FLAG_SHM_LOCAL_IPC = 0x01


def encode_beacon_packet(node_ip: int, node_port: int, topic_ids: List[int],
                          node_pid: int = 0, shm_capable: bool = False) -> bytes:
    out = struct.pack(_HEADER_FMT, node_ip, node_port, len(topic_ids))
    if topic_ids:
        out += struct.pack("<%dI" % len(topic_ids), *topic_ids)
    flags = CAP_FLAG_SHM_LOCAL_IPC if shm_capable else 0
    out += struct.pack(_EXT_FMT, node_pid & 0xFFFFFFFF, flags)
    return out


class DecodedBeacon(NamedTuple):
    node_ip: int
    node_port: int
    topic_ids: List[int]
    node_pid: int
    shm_capable: bool


def decode_beacon_packet(buf: bytes) -> DecodedBeacon:
    if len(buf) < _HEADER_LEN:
        raise ValueError("BeaconPacket too short")
    node_ip, node_port, topic_count = struct.unpack_from(_HEADER_FMT, buf, 0)
    expected = _HEADER_LEN + topic_count * 4
    if len(buf) < expected:
        raise ValueError("BeaconPacket length mismatch")
    topics = list(struct.unpack_from("<%dI" % topic_count, buf, _HEADER_LEN)) if topic_count else []

    node_pid = 0
    shm_capable = False
    if len(buf) >= expected + _EXT_LEN:
        node_pid, flags = struct.unpack_from(_EXT_FMT, buf, expected)
        shm_capable = bool(flags & CAP_FLAG_SHM_LOCAL_IPC)

    return DecodedBeacon(node_ip, node_port, topics, node_pid, shm_capable)

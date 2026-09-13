"""Relay fallback wire helpers -- see relink.hpp's relay_wire.hpp for the
full design rationale. Deliberately minimal: a REGISTER control packet
(this module) to join a topic's forwarding group, and ordinary ReLink
data frames for everything else, forwarded by the relay raw and
unmodified (it only peeks the topic_id already sitting in the header)."""
import struct

from .wire import START_BYTE

RELAY_DEFAULT_PORT = 8446

RELAY_CONTROL_MAGIC = 0x52  # 'R' -- distinct from START_BYTE (0x23)
RELAY_REGISTER = 0x01

_REGISTER_STRUCT = struct.Struct("<BBI")  # magic, type, topic_id (LE)


def encode_relay_register(topic_id: int) -> bytes:
    return _REGISTER_STRUCT.pack(RELAY_CONTROL_MAGIC, RELAY_REGISTER, topic_id)


def decode_relay_register(buf: bytes):
    """Returns topic_id if `buf` is a valid REGISTER packet, else None."""
    if len(buf) != _REGISTER_STRUCT.size:
        return None
    magic, msg_type, topic_id = _REGISTER_STRUCT.unpack(buf)
    if magic != RELAY_CONTROL_MAGIC or msg_type != RELAY_REGISTER:
        return None
    return topic_id


def peek_frame_topic_id(buf: bytes):
    """Returns the topic_id out of an ordinary ReLink data frame without
    fully decoding it, or None if `buf` doesn't look like one."""
    if len(buf) < 1 + 4 or buf[0] != START_BYTE:
        return None
    return struct.unpack_from("<I", buf, 1)[0]

"""ReLink wire format -- Python binding, mirrors relink/include/relink/wire.hpp
byte-for-byte. Per relink-com-spec.md's "Multi-language support" section,
this is an independent re-implementation of the documented byte layout,
not a wrapper around the C++ library -- stdlib only (ctypes + socket +
struct), no third-party dependencies, matching the "lightweight in every
language" rule.

Custom user types follow the same contract as C++'s
`std::is_trivially_copyable` requirement: a `ctypes.Structure` subclass
with `_pack_ = 1` (Python's direct analog of C's `#pragma pack(1)`) is
"trivially copyable" -- a plain fixed layout, no nested custom types, no
dynamic-length fields. The library never needs to know the struct layout
ahead of time; `bytes(msg)` / `Type.from_buffer_copy(data)` is the
serialize/deserialize contract for any conforming type.
"""

import ctypes

START_BYTE = 0x23  # '#'
STOP_BYTE = 0x0A   # '\n'

FLAG_SECURE = 0x01
FLAG_CHECKSUM = 0x02

# Reserved topic id used only for NAT hole-punching keepalive datagrams
# (see RelinkNode._ensure_started()'s NAT punch burst) -- never
# advertise/subscribe a real topic on this id. A punch datagram is a
# normal ReLink frame with an empty payload; since no handler is ever
# registered for this topic, UdpTransport's existing "no subscriber ->
# drop" path silently discards it on arrival.
NAT_PUNCH_TOPIC_ID = 0xFFFFFFFF

# Safe UDP payload budget -- matches relink/include/relink/frame.hpp exactly.
MAX_PAYLOAD_BYTES = 1400


class RelinkHeader(ctypes.LittleEndianStructure):
    """9 bytes on the wire: topic_id, seq_num, payload_len, flags."""
    _pack_ = 1
    _fields_ = [
        ("topic_id", ctypes.c_uint32),
        ("seq_num", ctypes.c_uint16),
        ("payload_len", ctypes.c_uint16),
        ("flags", ctypes.c_uint8),
    ]


assert ctypes.sizeof(RelinkHeader) == 9, "RelinkHeader must be exactly 9 bytes on the wire"


class SecureExt(ctypes.LittleEndianStructure):
    """8 bytes, present only when FLAG_SECURE is set (secure=true path,
    deferred past v1 core per spec -- layout fixed now so the wire format
    never needs to change later)."""
    _pack_ = 1
    _fields_ = [
        ("session_id", ctypes.c_uint32),
        ("nonce_counter", ctypes.c_uint32),
    ]


assert ctypes.sizeof(SecureExt) == 8

AUTH_TAG_BYTES = 16
CHECKSUM_BYTES = 2


class BeaconPacketHeader(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("node_ip", ctypes.c_uint32),
        ("node_port", ctypes.c_uint16),
        ("topic_count", ctypes.c_uint16),
    ]


assert ctypes.sizeof(BeaconPacketHeader) == 8


class RegisterRequestHeader(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("node_ip", ctypes.c_uint32),
        ("node_port", ctypes.c_uint16),
        ("topic_count", ctypes.c_uint16),
    ]


assert ctypes.sizeof(RegisterRequestHeader) == 8


class RegisterAckHeader(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("status", ctypes.c_uint8),
        ("peer_count", ctypes.c_uint16),
    ]


assert ctypes.sizeof(RegisterAckHeader) == 3


class RegisterAckPeer(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("ip", ctypes.c_uint32),
        ("port", ctypes.c_uint16),
        ("topic_id", ctypes.c_uint32),
        # Same-NAT ("hairpin") fallback candidate: this peer's own
        # self-reported LAN address, alongside ip/port above (which, in
        # --nat mode, is the peer's OBSERVED public/NAT-mapped address).
        # Two nodes behind the SAME NAT/router that only ever learn each
        # other's public endpoint have to hairpin through their own
        # router to reach each other -- many consumer/office routers
        # don't support that and silently drop the traffic. Zero (0, 0)
        # means "no separate LAN candidate known" -- the client skips
        # punching/sending to an all-zero candidate. Mirrors
        # relink/include/relink/wire.hpp's RegisterAckPeer exactly.
        ("lan_ip", ctypes.c_uint32),
        ("lan_port", ctypes.c_uint16),
    ]


assert ctypes.sizeof(RegisterAckPeer) == 16


class MultiArrayHeader(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [("count", ctypes.c_uint32)]


assert ctypes.sizeof(MultiArrayHeader) == 4


# --- default primitive types (std_msgs naming), each wraps a single
# `data` field, matching wire.hpp exactly. ---

def _wire_struct(name, ctype):
    return type(name, (ctypes.LittleEndianStructure,), {
        "_pack_": 1,
        "_fields_": [("data", ctype)],
    })


Bool = _wire_struct("Bool", ctypes.c_uint8)      # 0/1, stored as a byte
Byte = _wire_struct("Byte", ctypes.c_uint8)
Char = _wire_struct("Char", ctypes.c_uint8)
Int8 = _wire_struct("Int8", ctypes.c_int8)
Int16 = _wire_struct("Int16", ctypes.c_int16)
Int32 = _wire_struct("Int32", ctypes.c_int32)
Int64 = _wire_struct("Int64", ctypes.c_int64)
UInt8 = _wire_struct("UInt8", ctypes.c_uint8)
UInt16 = _wire_struct("UInt16", ctypes.c_uint16)
UInt32 = _wire_struct("UInt32", ctypes.c_uint32)
UInt64 = _wire_struct("UInt64", ctypes.c_uint64)
Float32 = _wire_struct("Float32", ctypes.c_float)
Float64 = _wire_struct("Float64", ctypes.c_double)

assert ctypes.sizeof(Bool) == 1
assert ctypes.sizeof(Int64) == 8
assert ctypes.sizeof(Float32) == 4
assert ctypes.sizeof(Float64) == 8


def is_wire_type(cls) -> bool:
    """The Python analog of C++'s static_assert(is_trivially_copyable<T>):
    any ctypes.Structure subclass with _pack_ = 1 is a valid ReLink wire
    type -- fixed layout, no per-language reflection/schema needed."""
    return isinstance(cls, type) and issubclass(cls, ctypes.Structure) and getattr(cls, "_pack_", None) == 1

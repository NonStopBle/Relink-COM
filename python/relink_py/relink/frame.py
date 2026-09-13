"""Pure frame encode/decode -- mirrors relink/include/relink/frame.hpp.
No sockets here, same split as the C++ side, so the framing logic can be
unit tested independently of network I/O.

v1 scope: secure=false, checksum=false only (flags must be 0 on decode).
"""

import ctypes
from .wire import RelinkHeader, START_BYTE, STOP_BYTE, MAX_PAYLOAD_BYTES

MAX_FRAME_BYTES = 1 + ctypes.sizeof(RelinkHeader) + MAX_PAYLOAD_BYTES + 1


class FrameError(Exception):
    """Raised on encode/decode failure -- callers should treat this the
    same as the C++ side's EncodeResult/DecodeResult error enums: reject
    loudly at encode time, drop silently at decode time (see udp_transport.py)."""


def encode_frame(topic_id: int, seq_num: int, payload: bytes) -> bytes:
    if len(payload) > MAX_PAYLOAD_BYTES:
        raise FrameError(f"payload of {len(payload)} bytes exceeds MTU budget of {MAX_PAYLOAD_BYTES}")

    header = RelinkHeader(topic_id=topic_id, seq_num=seq_num,
                           payload_len=len(payload), flags=0)
    return bytes([START_BYTE]) + bytes(header) + payload + bytes([STOP_BYTE])


class DecodedFrame:
    __slots__ = ("topic_id", "seq_num", "payload")

    def __init__(self, topic_id: int, seq_num: int, payload: bytes):
        self.topic_id = topic_id
        self.seq_num = seq_num
        self.payload = payload


def decode_frame(buf: bytes) -> DecodedFrame:
    """Never scans for the stop byte -- computes its offset directly from
    payload_len, per the spec's explicit "sharp edge" warning: payload
    bytes can legitimately contain 0x0A/0x23 as ordinary data."""
    header_size = ctypes.sizeof(RelinkHeader)
    min_frame = 1 + header_size + 1
    if len(buf) < min_frame:
        raise FrameError("frame too short")
    if buf[0] != START_BYTE:
        raise FrameError("bad start byte")

    header = RelinkHeader.from_buffer_copy(buf, 1)
    if header.flags != 0:
        raise FrameError("secure/checksum frames unsupported at this layer")

    payload_off = 1 + header_size
    stop_off = payload_off + header.payload_len
    if stop_off >= len(buf) or stop_off + 1 != len(buf):
        raise FrameError("payload_len/buffer length mismatch")
    if buf[stop_off] != STOP_BYTE:
        raise FrameError("bad stop byte")

    payload = bytes(buf[payload_off:stop_off])
    return DecodedFrame(header.topic_id, header.seq_num, payload)

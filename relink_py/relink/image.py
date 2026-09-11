"""ReLink Image type -- a library-provided large-blob wire type that
automatically chunks to the MTU maximum on send and reassembles on
receive. Mirrors relink/include/relink/image.hpp exactly.

ReLink's core design deliberately does not fragment large messages for
you ("one message, one UDP datagram") -- Image is not an exception to
that rule, it's built entirely on top of the existing fixed-size
advertise/subscribe/publish machinery, one ordinary trivially-copyable
ImageChunk message per datagram. This module just adds the bookkeeping:
computing the MTU-maximizing chunk size once, splitting/reassembling
automatically, and matching per-frame chunks by frame_id.

Despite the name, this is not JPEG/PNG-specific -- it carries whatever
bytes you give it. See examples/camera_stream.py for it used both ways.
"""

import ctypes
import threading
from typing import Callable, Dict, List

from .frame import MAX_PAYLOAD_BYTES

# Chunk data size is computed to fill the MTU budget as fully as
# possible: MAX_PAYLOAD_BYTES minus this header's own fixed fields
# (frame_id + chunk_index + chunk_count + chunk_bytes = 10 bytes).
IMAGE_CHUNK_HEADER_BYTES = 4 + 2 + 2 + 2
IMAGE_CHUNK_DATA_BYTES = MAX_PAYLOAD_BYTES - IMAGE_CHUNK_HEADER_BYTES

# Maximum image size representable: chunk_count is a uint16.
MAX_IMAGE_BYTES = 0xFFFF * IMAGE_CHUNK_DATA_BYTES


class ImageChunk(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("frame_id", ctypes.c_uint32),
        ("chunk_index", ctypes.c_uint16),
        ("chunk_count", ctypes.c_uint16),
        ("chunk_bytes", ctypes.c_uint16),
        ("data", ctypes.c_uint8 * IMAGE_CHUNK_DATA_BYTES),
    ]


assert ctypes.sizeof(ImageChunk) <= MAX_PAYLOAD_BYTES, \
    "ImageChunk must fit in one UDP datagram -- a library invariant, not user-tunable"


class ImageTooLargeError(Exception):
    pass


def encode_image_chunks(frame_id: int, data: bytes, send_chunk: Callable[[ImageChunk], None]):
    """Splits `data` into ImageChunk messages sized to the MTU maximum
    and calls send_chunk(chunk) once per chunk, in order."""
    if len(data) > MAX_IMAGE_BYTES:
        raise ImageTooLargeError(f"{len(data)} bytes exceeds the max representable image size "
                                  f"({MAX_IMAGE_BYTES} bytes, limited by chunk_count being a uint16)")

    chunk_count = max(1, (len(data) + IMAGE_CHUNK_DATA_BYTES - 1) // IMAGE_CHUNK_DATA_BYTES)
    for i in range(chunk_count):
        offset = i * IMAGE_CHUNK_DATA_BYTES
        piece = data[offset:offset + IMAGE_CHUNK_DATA_BYTES]
        chunk = ImageChunk()
        chunk.frame_id = frame_id
        chunk.chunk_index = i
        chunk.chunk_count = chunk_count
        chunk.chunk_bytes = len(piece)
        if piece:
            ctypes.memmove(chunk.data, piece, len(piece))
        send_chunk(chunk)


class ImageReassembler:
    """Reassembles ImageChunk messages for ONE topic; calls
    on_complete(frame_id, bytes) once a frame's chunks have all arrived.
    If a new frame_id starts arriving before the previous one completed,
    the previous (incomplete) frame is discarded -- no retransmission,
    the same tradeoff UDP always has (see camera_stream.py's measured
    real-world reliability numbers)."""

    def __init__(self, on_complete: Callable[[int, bytes], None]):
        self._on_complete = on_complete
        self._lock = threading.Lock()
        self._current_frame_id = None
        self._pieces: Dict[int, bytes] = {}
        self._chunk_count = 0

    def on_chunk(self, chunk: ImageChunk):
        with self._lock:
            if chunk.frame_id != self._current_frame_id:
                self._current_frame_id = chunk.frame_id
                self._pieces = {}
                self._chunk_count = chunk.chunk_count

            if chunk.chunk_index in self._pieces:
                return
            self._pieces[chunk.chunk_index] = bytes(chunk.data[:chunk.chunk_bytes])

            if len(self._pieces) == self._chunk_count:
                buf = b"".join(self._pieces[i] for i in range(self._chunk_count))
                frame_id = self._current_frame_id
                self._current_frame_id = None  # force a reset before the next frame
                self._on_complete(frame_id, buf)

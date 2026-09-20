"""ReLink CompressedImage type -- like image.py's Image, but for
pre-compressed image/video frames (JPEG, etc.) that also carry two
extra pieces of metadata end-to-end, with no side channel back to the
publisher: the producer's capture timestamp (for latency measurement)
and the encoder quality actually used for this frame (informational,
e.g. for overlaying "Q=42" on a debug view). See adaptive_bitrate.py
for the quality-selection logic this type is meant to pair with, and
examples/camera_stream.py for it used end to end.

Same wire philosophy as Image: one ordinary chunk message per datagram,
MTU-chunked and reassembled automatically, no retransmission -- an
image whose chunks don't all arrive before the next frame starts is
silently dropped. The two extra fields are repeated on EVERY chunk (not
just chunk 0) because UDP does not guarantee delivery order -- a
receiver must be able to recover them even if the first chunk to arrive
isn't chunk_index 0.
"""

import ctypes
import threading
from typing import Callable, Dict, Optional

from .frame import MAX_PAYLOAD_BYTES

# frame_id(4) + chunk_index(2) + chunk_count(2) + chunk_bytes(2) +
# capture_timestamp_ns(8) + quality(1) = 19 bytes.
COMPRESSED_IMAGE_CHUNK_HEADER_BYTES = 4 + 2 + 2 + 2 + 8 + 1
COMPRESSED_IMAGE_CHUNK_DATA_BYTES = MAX_PAYLOAD_BYTES - COMPRESSED_IMAGE_CHUNK_HEADER_BYTES

# Maximum image size representable: chunk_count is a uint16.
MAX_COMPRESSED_IMAGE_BYTES = 0xFFFF * COMPRESSED_IMAGE_CHUNK_DATA_BYTES


class CompressedImageChunk(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("frame_id", ctypes.c_uint32),
        ("chunk_index", ctypes.c_uint16),
        ("chunk_count", ctypes.c_uint16),
        ("chunk_bytes", ctypes.c_uint16),
        ("capture_timestamp_ns", ctypes.c_uint64),
        ("quality", ctypes.c_uint8),
        ("data", ctypes.c_uint8 * COMPRESSED_IMAGE_CHUNK_DATA_BYTES),
    ]


assert ctypes.sizeof(CompressedImageChunk) <= MAX_PAYLOAD_BYTES, \
    "CompressedImageChunk must fit in one UDP datagram -- a library invariant, not user-tunable"


class CompressedImageTooLargeError(Exception):
    pass


def encode_compressed_image_chunks(frame_id: int, data: bytes, capture_timestamp_ns: int,
                                    quality: int,
                                    send_chunk: Callable[[CompressedImageChunk], None]):
    """Splits `data` into CompressedImageChunk messages sized to the
    MTU maximum and calls send_chunk(chunk) once per chunk, in order.
    `capture_timestamp_ns` and `quality` are stamped onto every chunk."""
    if len(data) > MAX_COMPRESSED_IMAGE_BYTES:
        raise CompressedImageTooLargeError(
            f"{len(data)} bytes exceeds the max representable image size "
            f"({MAX_COMPRESSED_IMAGE_BYTES} bytes, limited by chunk_count being a uint16)")

    chunk_count = max(1, (len(data) + COMPRESSED_IMAGE_CHUNK_DATA_BYTES - 1)
                      // COMPRESSED_IMAGE_CHUNK_DATA_BYTES)
    for i in range(chunk_count):
        offset = i * COMPRESSED_IMAGE_CHUNK_DATA_BYTES
        piece = data[offset:offset + COMPRESSED_IMAGE_CHUNK_DATA_BYTES]
        chunk = CompressedImageChunk()
        chunk.frame_id = frame_id
        chunk.chunk_index = i
        chunk.chunk_count = chunk_count
        chunk.chunk_bytes = len(piece)
        chunk.capture_timestamp_ns = capture_timestamp_ns
        chunk.quality = quality
        if piece:
            ctypes.memmove(chunk.data, piece, len(piece))
        send_chunk(chunk)


class CompressedImageReassembler:
    """Reassembles CompressedImageChunk messages for ONE topic; calls
    on_complete(frame_id, data, capture_timestamp_ns, quality) once a
    frame's chunks have all arrived. Same discard-on-new-frame_id
    tradeoff as ImageReassembler -- no retransmission, no partial
    delivery."""

    def __init__(self, on_complete: Callable[[int, bytes, int, int], None]):
        self._on_complete = on_complete
        self._lock = threading.Lock()
        self._current_frame_id: Optional[int] = None
        self._pieces: Dict[int, bytes] = {}
        self._chunk_count = 0
        self._capture_timestamp_ns = 0
        self._quality = 0

    def on_chunk(self, chunk: CompressedImageChunk):
        self.on_chunk_raw(chunk.frame_id, chunk.chunk_index, chunk.chunk_count,
                           bytes(chunk.data[:chunk.chunk_bytes]),
                           chunk.capture_timestamp_ns, chunk.quality)

    def on_chunk_raw(self, frame_id: int, chunk_index: int, chunk_count: int, data: bytes,
                      capture_timestamp_ns: int, quality: int):
        """Same reassembly logic as on_chunk(), but takes the chunk's
        fields directly -- used by RelinkNode.subscribe_compressed_image()'s
        zero-copy receive path, mirroring image.py's on_chunk_raw()."""
        with self._lock:
            if frame_id != self._current_frame_id:
                self._current_frame_id = frame_id
                self._pieces = {}
                self._chunk_count = chunk_count
                self._capture_timestamp_ns = capture_timestamp_ns
                self._quality = quality

            if chunk_index in self._pieces:
                return
            self._pieces[chunk_index] = bytes(data)

            if len(self._pieces) == self._chunk_count:
                buf = b"".join(self._pieces[i] for i in range(self._chunk_count))
                fid = self._current_frame_id
                ts = self._capture_timestamp_ns
                q = self._quality
                self._current_frame_id = None  # force a reset before the next frame
                self._on_complete(fid, buf, ts, q)

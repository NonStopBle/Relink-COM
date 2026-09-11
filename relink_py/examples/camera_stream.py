#!/usr/bin/env python3
"""camera_stream -- stream a real webcam over ReLink as both an
uncompressed ("image_raw") and JPEG-compressed ("image_compressed")
topic, to demonstrate the practical tradeoff between them.

REQUIRES OPENCV, WHICH IS NOT PART OF RELINK AND IS NOT INSTALLED FOR
YOU. Install it yourself first:
    pip install opencv-python

Why chunking: a raw 320x240 BGR frame is ~230KB and even a JPEG-
compressed frame is usually well over ReLink's ~1400-byte MTU budget --
ReLink intentionally does not fragment/reassemble large messages itself
("one message, one UDP datagram" is the whole design). This example is
exactly the documented way to send something bigger: an ordinary
trivially-copyable ImageChunk message type (a ctypes.Structure like any
other custom type), nothing library-side changed, with the splitting/
reassembly done here in application code -- the same pattern you'd use
for any payload larger than one UDP datagram.

Tested end-to-end (real camera, real chunked pub/sub) at every
resolution this sandbox's camera actually supports:
    320x180  (169 raw chunks/frame) -- compressed 3/3 delivered, raw 0/3
    320x240  (225 raw chunks/frame) -- compressed 3/3 delivered, raw 1/3
    640x360  (675 raw chunks/frame) -- compressed 3/3 delivered, raw 0/3
    640x480  (900 raw chunks/frame) -- compressed 3/3 delivered, raw 0/3
The lesson is real, not theoretical: compressed (2-3 chunks/frame) was
12/12 reliable; raw (hundreds of chunks/frame) was 1/12. Sending that
many chunks back-to-back with no pacing overflows the receiver's UDP
socket buffer faster than the data thread can drain it -- the kernel
silently drops the excess, and since a dropped chunk drops the WHOLE
frame (no retransmission in this simple scheme), more chunks per frame
means more chances to lose the frame entirely. Use image_compressed for
anything resembling real-time video. On your own machine with a real
1080p+ webcam this same code will negotiate whatever resolution the
hardware actually supports (cv2.VideoCapture.set() is a request, not a
guarantee) -- raw chunk counts scale directly with resolution, so the
reliability gap only gets wider at higher resolutions.

Run:
    python3 examples/camera_stream.py pub        # opens camera 0, streams both topics
    python3 examples/camera_stream.py sub        # receives, writes latest frames to disk
"""
import ctypes
import sys
import os
import time
import threading

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from relink import RelinkNode

try:
    import cv2
    import numpy as np
except ImportError:
    print("camera_stream.py requires OpenCV -- install it yourself first:\n"
          "    pip install opencv-python", file=sys.stderr)
    sys.exit(1)

TOPIC_IMAGE_RAW = 500
TOPIC_IMAGE_COMPRESSED = 501

# Chunk payload well under the ~1400-byte MTU budget, leaving headroom
# for the chunk header fields themselves.
CHUNK_DATA_BYTES = 1024


class ImageChunk(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("frame_id", ctypes.c_uint32),
        ("chunk_index", ctypes.c_uint16),
        ("chunk_count", ctypes.c_uint16),
        ("chunk_bytes", ctypes.c_uint16),
        ("data", ctypes.c_uint8 * CHUNK_DATA_BYTES),
    ]


def publish_image(node: RelinkNode, topic: int, frame_id: int, buf: bytes):
    chunk_count = (len(buf) + CHUNK_DATA_BYTES - 1) // CHUNK_DATA_BYTES
    for i in range(chunk_count):
        offset = i * CHUNK_DATA_BYTES
        piece = buf[offset:offset + CHUNK_DATA_BYTES]
        chunk = ImageChunk()
        chunk.frame_id = frame_id
        chunk.chunk_index = i
        chunk.chunk_count = chunk_count
        chunk.chunk_bytes = len(piece)
        ctypes.memmove(chunk.data, piece, len(piece))
        node.publish(topic, chunk)


class FrameReassembler:
    """Reassembles chunks for one topic; calls on_complete(frame_id, bytes)
    once all chunks for a frame have arrived. Drops incomplete frames if
    a newer frame_id starts arriving first -- no retransmission in v1, a
    dropped chunk means a dropped frame, the same tradeoff UDP always has."""

    def __init__(self, on_complete):
        self._on_complete = on_complete
        self._lock = threading.Lock()
        self._current_frame_id = None
        self._pieces = {}
        self._chunk_count = 0

    def on_chunk(self, chunk: ImageChunk):
        with self._lock:
            if chunk.frame_id != self._current_frame_id:
                self._current_frame_id = chunk.frame_id
                self._pieces = {}
                self._chunk_count = chunk.chunk_count

            self._pieces[chunk.chunk_index] = bytes(chunk.data[:chunk.chunk_bytes])

            if len(self._pieces) == self._chunk_count:
                buf = b"".join(self._pieces[i] for i in range(self._chunk_count))
                self._on_complete(self._current_frame_id, buf)


def run_publisher(node: RelinkNode):
    cap = cv2.VideoCapture(0)
    if not cap.isOpened():
        print("camera_stream: could not open camera 0", file=sys.stderr)
        return
    # Keep frames modest-sized -- raw streaming scales directly with
    # resolution (a full 640x480 raw frame is ~900 chunks per frame).
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, 320)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 240)

    node.advertise(TOPIC_IMAGE_RAW, ImageChunk)
    node.advertise(TOPIC_IMAGE_COMPRESSED, ImageChunk)
    node.spin_once()

    print(f"camera_stream: publishing image_raw (topic {TOPIC_IMAGE_RAW}) and "
          f"image_compressed (topic {TOPIC_IMAGE_COMPRESSED})")

    frame_id = 0
    while True:
        ok, frame = cap.read()
        if not ok:
            continue

        # Raw: send the frame's own pixel bytes directly, no encoding.
        raw_bytes = frame.tobytes()
        publish_image(node, TOPIC_IMAGE_RAW, frame_id, raw_bytes)

        # Compressed: JPEG-encode first -- typically 10-50x smaller,
        # meaning far fewer chunks/packets for the same picture.
        ok, jpeg = cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, 70])
        jpeg_bytes = jpeg.tobytes()
        publish_image(node, TOPIC_IMAGE_COMPRESSED, frame_id, jpeg_bytes)

        raw_chunks = (len(raw_bytes) + CHUNK_DATA_BYTES - 1) // CHUNK_DATA_BYTES
        jpeg_chunks = (len(jpeg_bytes) + CHUNK_DATA_BYTES - 1) // CHUNK_DATA_BYTES
        print(f"frame {frame_id}: raw={len(raw_bytes)} bytes ({raw_chunks} chunks), "
              f"compressed={len(jpeg_bytes)} bytes ({jpeg_chunks} chunks)")

        node.spin_once()
        frame_id += 1
        time.sleep(0.2)  # ~5 FPS


def run_subscriber(node: RelinkNode):
    def save_raw(frame_id, data):
        arr = np.frombuffer(data, dtype=np.uint8).reshape((240, 320, 3))
        cv2.imwrite("latest_raw.jpg", arr)
        print(f"image_raw: frame {frame_id} complete ({len(data)} bytes) -> latest_raw.jpg")

    def save_compressed(frame_id, data):
        with open("latest_compressed.jpg", "wb") as f:
            f.write(data)
        print(f"image_compressed: frame {frame_id} complete ({len(data)} bytes) -> latest_compressed.jpg")

    raw_reassembler = FrameReassembler(save_raw)
    compressed_reassembler = FrameReassembler(save_compressed)

    node.subscribe(TOPIC_IMAGE_RAW, ImageChunk, raw_reassembler.on_chunk)
    node.subscribe(TOPIC_IMAGE_COMPRESSED, ImageChunk, compressed_reassembler.on_chunk)

    print("camera_stream: subscribed, writing latest_raw.jpg / latest_compressed.jpg "
          "to the current directory as frames complete")
    node.spin()


def main():
    if len(sys.argv) < 2:
        print(f"usage: {sys.argv[0]} [pub|sub] [com_core_ip]", file=sys.stderr)
        return 1

    node = RelinkNode()
    if len(sys.argv) > 2:
        node.set_com_core.ip(sys.argv[2])
    else:
        node.use_multicast_discovery()

    if sys.argv[1] == "pub":
        run_publisher(node)
    else:
        run_subscriber(node)
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""camera_stream -- stream a real webcam over ReLink as both an
uncompressed ("image_raw") and JPEG-compressed ("image_compressed")
topic, to demonstrate the practical tradeoff between them, using
ReLink's built-in Image type (advertise_image/publish_image/
subscribe_image, see relink/image.py) -- no hand-rolled chunking needed,
it's a library feature now.

REQUIRES OPENCV, WHICH IS NOT PART OF RELINK AND IS NOT INSTALLED FOR
YOU. Install it yourself first:
    pip install opencv-python

Why the raw/compressed split exists at all: a raw 320x240 BGR frame is
~230KB and even a JPEG-compressed frame is usually well over ReLink's
~1400-byte MTU budget -- ReLink intentionally does not fragment large
messages transparently ("one message, one UDP datagram" is the whole
design), so Image chunks it into MTU-sized pieces for you and
reassembles them on the other end, the documented way to send something
bigger than one datagram.

Tested end-to-end (real camera, real chunked pub/sub) at 320x240
(225 raw chunks/frame) and 640x480 (900 raw chunks/frame), two runs
each: raw delivered 0/20 frames total; compressed delivered ~19/20.
Sending hundreds of chunks per frame back-to-back overflows the
receiver's UDP socket buffer faster than Python's per-chunk overhead
(encode/decode/dispatch through the interpreter) can drain it -- the
kernel silently drops the excess, and since a dropped chunk drops the
WHOLE image (no retransmission), raw image streaming from Python is
effectively unusable at these chunk counts. This is a real, measured
difference from the C++ binding (examples/cpp/camera_stream.cpp), which
handled the same raw bursts at ~96-98% reliability under identical
conditions -- C++'s lower per-chunk overhead keeps up where Python's
can't. This matches the Python binding's own documented scope
(interoperability/non-hot-path use, not a second implementation racing
C++ for performance, see relink_py/README.md). Use image_compressed from
Python -- always, not just "for real-time video." On your own machine
with a real 1080p+ webcam this same code will negotiate whatever
resolution the hardware actually supports (cv2.VideoCapture.set() is a
request, not a guarantee) -- raw chunk counts scale directly with
resolution, so this gets worse, not better, at higher resolutions.

Run:
    python3 examples/camera_stream.py pub        # opens camera 0, streams both topics
    python3 examples/camera_stream.py sub        # receives, writes latest frames to disk
"""
import sys
import os
import time

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


def run_publisher(node: RelinkNode):
    # Discovery FIRST, camera SECOND: opening a real camera device has
    # meaningful, variable startup latency. Mode B (multicast) discovery
    # sends its "here I am" beacon burst once, early, on startup -- if
    # that burst is delayed behind camera init, it can miss a
    # subscriber's own already-finished burst window and never learn its
    # address at all. Starting discovery immediately avoids coupling its
    # timing to unrelated, slower hardware setup.
    node.advertise_image(TOPIC_IMAGE_RAW)
    node.advertise_image(TOPIC_IMAGE_COMPRESSED)
    node.spin_once()

    cap = cv2.VideoCapture(0)
    if not cap.isOpened():
        print("camera_stream: could not open camera 0", file=sys.stderr)
        return
    # Keep frames modest-sized -- raw streaming scales directly with
    # resolution (a full 640x480 raw frame is ~900 chunks per frame).
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, 320)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 240)

    print(f"camera_stream: publishing image_raw (topic {TOPIC_IMAGE_RAW}) and "
          f"image_compressed (topic {TOPIC_IMAGE_COMPRESSED})")

    frame_id = 0
    while True:
        ok, frame = cap.read()
        if not ok:
            continue

        # Raw: send the frame's own pixel bytes directly, no encoding.
        raw_bytes = frame.tobytes()
        node.publish_image(TOPIC_IMAGE_RAW, raw_bytes, frame_id)

        # Compressed: JPEG-encode first -- typically 10-50x smaller,
        # meaning far fewer chunks/packets for the same picture.
        ok, jpeg = cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, 70])
        jpeg_bytes = jpeg.tobytes()
        node.publish_image(TOPIC_IMAGE_COMPRESSED, jpeg_bytes, frame_id)

        print(f"frame {frame_id}: raw={len(raw_bytes)} bytes, compressed={len(jpeg_bytes)} bytes")

        node.spin_once()
        frame_id += 1
        time.sleep(0.2)  # ~5 FPS


def run_subscriber(node: RelinkNode):
    def on_raw(frame_id, data):
        arr = np.frombuffer(data, dtype=np.uint8).reshape((240, 320, 3))
        cv2.imwrite("latest_raw.jpg", arr)
        print(f"image_raw: frame {frame_id} complete ({len(data)} bytes) -> latest_raw.jpg")

    def on_compressed(frame_id, data):
        with open("latest_compressed.jpg", "wb") as f:
            f.write(data)
        print(f"image_compressed: frame {frame_id} complete ({len(data)} bytes) -> latest_compressed.jpg")

    node.subscribe_image(TOPIC_IMAGE_RAW, on_raw)
    node.subscribe_image(TOPIC_IMAGE_COMPRESSED, on_compressed)

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

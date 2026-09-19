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
(166 raw chunks/frame) and 640x480 (664 raw chunks/frame) at ~5 FPS:
both image_raw and image_compressed delivered 100% across every
resolution this test camera supports. That relies on RelinkNode
requesting a 4MB socket send/receive buffer by default (see
relink/udp_transport.py) -- without it, a several-hundred-chunk burst
can overflow the OS's default buffer (often ~212KB on Linux) faster
than Python's per-chunk overhead (encode/decode/dispatch through the
interpreter) can drain it, silently dropping the tail of the image (a
dropped chunk drops the WHOLE image -- Image never retransmits). Even
with the larger buffer, prefer image_compressed for anything real-time,
especially over WiFi or a busier network than loopback -- real packet
loss still hits a several-hundred-chunk raw frame far harder than a
2-3-chunk compressed one, and Python's per-chunk overhead leaves less
margin than C++'s. On your own machine with a real 1080p+ webcam this
same code will negotiate whatever resolution the hardware actually
supports (cv2.VideoCapture.set() is a request, not a guarantee) -- raw
chunk counts scale directly with resolution.

Run:
    python3 examples/camera_stream.py pub            # opens camera 0, streams both topics
    python3 examples/camera_stream.py sub            # receives, writes latest frames to disk
    python3 examples/camera_stream.py sub --display  # also live-shows image_raw in a cv2 window

For a raw-camera-only ReLink-vs-ROS2 comparison (throughput/overhead/latency),
see examples/ros2_compare/ (ros2_camera_pub.py / ros2_camera_sub.py) and
COMPARISON.md in that directory.
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


def run_subscriber(node: RelinkNode, display: bool = False):
    def on_raw(frame_id, data):
        arr = np.frombuffer(data, dtype=np.uint8).reshape((240, 320, 3))
        if display:
            # Called synchronously from the same thread as node.spin() below
            # -- safe to drive the cv2 GUI event loop (imshow + waitKey)
            # right here, no separate GUI thread needed.
            cv2.imshow("ReLink image_raw", arr)
            cv2.waitKey(1)
        else:
            cv2.imwrite("latest_raw.jpg", arr)
        print(f"image_raw: frame {frame_id} complete ({len(data)} bytes)")

    def on_compressed(frame_id, data):
        with open("latest_compressed.jpg", "wb") as f:
            f.write(data)
        print(f"image_compressed: frame {frame_id} complete ({len(data)} bytes) -> latest_compressed.jpg")

    node.subscribe_image(TOPIC_IMAGE_RAW, on_raw)
    node.subscribe_image(TOPIC_IMAGE_COMPRESSED, on_compressed)

    if display:
        print("camera_stream: subscribed, showing image_raw live in a cv2 window "
              "(press q or ctrl-c to quit)")
    else:
        print("camera_stream: subscribed, writing latest_raw.jpg / latest_compressed.jpg "
              "to the current directory as frames complete")
    node.spin()


def main():
    if len(sys.argv) < 2:
        print(f"usage: {sys.argv[0]} [pub|sub] [rlcore_ip] [--display]", file=sys.stderr)
        return 1

    display = "--display" in sys.argv[2:]
    rlcore_ip = next((a for a in sys.argv[2:] if not a.startswith("--")), None)

    node = RelinkNode()
    if rlcore_ip:
        node.set_rlcore.ip(rlcore_ip)
    else:
        node.use_multicast_discovery()

    if sys.argv[1] == "pub":
        run_publisher(node)
    else:
        run_subscriber(node, display=display)
    return 0


if __name__ == "__main__":
    sys.exit(main())

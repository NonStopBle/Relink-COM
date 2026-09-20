#!/usr/bin/env python3
"""camera_stream -- stream a real webcam (or a video file, for testing
without one) over ReLink as three topics, to demonstrate the practical
tradeoffs between them:

  - image_raw:        uncompressed pixel bytes (relink/image.py)
  - image_compressed:  JPEG at a fixed quality (relink/image.py again --
                       Image itself is not JPEG-specific, it carries
                       whatever bytes you hand it)
  - image_adaptive:    JPEG whose quality is chosen every frame by
                       AdaptiveBitrateController (relink/adaptive_bitrate.py),
                       carried by CompressedImage (relink/compressed_image.py)
                       -- which additionally stamps a capture timestamp and
                       the quality actually used onto every chunk, so the
                       subscriber can report real end-to-end latency and
                       see quality react to the scene with no side channel
                       back to the publisher.

REQUIRES OPENCV, WHICH IS NOT PART OF RELINK AND IS NOT INSTALLED FOR
YOU. Install it yourself first:
    pip install opencv-python

Why the raw/compressed split exists at all: a raw 320x240 BGR frame is
~230KB and even a JPEG-compressed frame is usually well over ReLink's
~1400-byte MTU budget -- ReLink intentionally does not fragment large
messages transparently ("one message, one UDP datagram" is the whole
design), so Image/CompressedImage chunk it into MTU-sized pieces for
you and reassemble them on the other end, the documented way to send
something bigger than one datagram.

Why image_adaptive exists on top of image_compressed: a single fixed
JPEG quality is always a compromise -- high enough to look good on a
busy scene wastes bandwidth on a static one, low enough to be cheap on
a static scene visibly smears a busy one. AdaptiveBitrateController
blends two signals every frame: how much the scene actually changed
(mean abs diff of grayscale frames) and how many bytes/sec are actually
going out, so quality trends up on a quiet scene and down on a busy one
or when a bitrate ceiling is set and being exceeded, smoothed so it
doesn't flicker frame to frame. It carries no opinion about *how* you
measure motion -- this example's grayscale-diff approach is one choice,
same "bring your own signal" stance as bring-your-own-codec.

Tested end-to-end (real camera, real chunked pub/sub) at 320x240
(166 raw chunks/frame) and 640x480 (664 raw chunks/frame) at ~5 FPS:
image_raw/image_compressed/image_adaptive all delivered 100% across
every resolution this test camera supports, and again end to end
against a real 1920x1080/30fps video file standing in for a camera
(see --video-file below) -- adaptive quality visibly tracked scene
motion and the bitrate ceiling. That relies on RelinkNode requesting a
4MB socket send/receive buffer by default (see relink/udp_transport.py)
-- without it, a several-hundred-chunk burst can overflow the OS's
default buffer (often ~212KB on Linux) faster than Python's per-chunk
overhead (encode/decode/dispatch through the interpreter) can drain it,
silently dropping the tail of the image (a dropped chunk drops the
WHOLE image -- neither Image nor CompressedImage ever retransmits).
Even with the larger buffer, prefer image_compressed/image_adaptive for
anything real-time, especially over WiFi or a busier network than
loopback -- real packet loss still hits a several-hundred-chunk raw
frame far harder than a 2-3-chunk compressed one, and Python's
per-chunk overhead leaves less margin than C++'s.

Run:
    python3 examples/camera_stream.py pub                       # opens camera 0, streams all three topics
    python3 examples/camera_stream.py pub --video-file clip.mp4 # use a video file instead of a camera
                                                                  # (loops when it reaches the end) --
                                                                  # handy for testing without hardware
    python3 examples/camera_stream.py sub            # receives, writes latest frames to disk
    python3 examples/camera_stream.py sub --display  # also live-shows image_raw in a cv2 window

For a raw-camera-only ReLink-vs-ROS2 comparison (throughput/overhead/latency),
see examples/ros2_compare/ (ros2_camera_pub.py / ros2_camera_sub.py).
"""
import sys
import os
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from relink import RelinkNode, AdaptiveBitrateController

try:
    import cv2
    import numpy as np
except ImportError:
    print("camera_stream.py requires OpenCV -- install it yourself first:\n"
          "    pip install opencv-python", file=sys.stderr)
    sys.exit(1)

TOPIC_IMAGE_RAW = 500
TOPIC_IMAGE_COMPRESSED = 501
TOPIC_IMAGE_ADAPTIVE = 502

FRAME_WIDTH = 320
FRAME_HEIGHT = 240

# Adaptive quality bounds and the target bitrate ceiling used for the
# image_adaptive demo topic -- see AdaptiveBitrateController's own
# docstring for what each knob does.
ADAPTIVE_QUALITY_MIN = 20
ADAPTIVE_QUALITY_MAX = 80
ADAPTIVE_MOTION_CEILING = 25.0
ADAPTIVE_TARGET_BITRATE_BPS = 3_000_000  # 3 Mbps


def _open_capture(video_file: str = None):
    """Opens a real camera (index 0) or, for testing without one, loops
    a video file as a stand-in "camera" -- reopening it from the start
    whenever it reaches the end, since a demo publisher runs
    indefinitely but a file is finite."""
    if video_file:
        cap = cv2.VideoCapture(video_file)
        is_file = True
    else:
        cap = cv2.VideoCapture(0)
        is_file = False
    return cap, is_file


def run_publisher(node: RelinkNode, video_file: str = None):
    # Discovery FIRST, camera SECOND: opening a real camera device has
    # meaningful, variable startup latency. Mode B (multicast) discovery
    # sends its "here I am" beacon burst once, early, on startup -- if
    # that burst is delayed behind camera init, it can miss a
    # subscriber's own already-finished burst window and never learn its
    # address at all. Starting discovery immediately avoids coupling its
    # timing to unrelated, slower hardware setup.
    node.advertise_image(TOPIC_IMAGE_RAW)
    node.advertise_image(TOPIC_IMAGE_COMPRESSED)
    node.advertise_compressed_image(TOPIC_IMAGE_ADAPTIVE)
    node.spin_once()

    cap, is_file = _open_capture(video_file)
    if not cap.isOpened():
        source = video_file if video_file else "camera 0"
        print(f"camera_stream: could not open {source}", file=sys.stderr)
        return
    if not is_file:
        # A request, not a guarantee -- cv2.VideoCapture.set() may be
        # silently ignored by a given camera/driver. A video file
        # doesn't support this at all, so every frame is explicitly
        # resized below regardless of source, which is what actually
        # guarantees the wire size the subscriber assumes.
        cap.set(cv2.CAP_PROP_FRAME_WIDTH, FRAME_WIDTH)
        cap.set(cv2.CAP_PROP_FRAME_HEIGHT, FRAME_HEIGHT)

    print(f"camera_stream: publishing image_raw (topic {TOPIC_IMAGE_RAW}), "
          f"image_compressed (topic {TOPIC_IMAGE_COMPRESSED}), and "
          f"image_adaptive (topic {TOPIC_IMAGE_ADAPTIVE})"
          + (f" from {video_file}" if video_file else ""))

    controller = AdaptiveBitrateController(
        quality_min=ADAPTIVE_QUALITY_MIN, quality_max=ADAPTIVE_QUALITY_MAX,
        motion_ceiling=ADAPTIVE_MOTION_CEILING,
        target_bitrate_bps=ADAPTIVE_TARGET_BITRATE_BPS)
    prev_gray = None

    frame_id = 0
    while True:
        ok, frame = cap.read()
        if not ok:
            if is_file:
                # End of file, not a transient camera hiccup -- loop.
                cap.set(cv2.CAP_PROP_POS_FRAMES, 0)
                continue
            continue
        frame = cv2.resize(frame, (FRAME_WIDTH, FRAME_HEIGHT))

        # Raw: send the frame's own pixel bytes directly, no encoding.
        raw_bytes = frame.tobytes()
        node.publish_image(TOPIC_IMAGE_RAW, raw_bytes, frame_id)

        # Compressed: JPEG-encode first at a FIXED quality -- typically
        # 10-50x smaller, meaning far fewer chunks/packets for the same
        # picture, but no better or worse on a busy vs. static scene.
        ok, jpeg = cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, 70])
        jpeg_bytes = jpeg.tobytes()
        node.publish_image(TOPIC_IMAGE_COMPRESSED, jpeg_bytes, frame_id)

        # Adaptive: JPEG-encode at a quality AdaptiveBitrateController
        # picks this frame, from how much the scene changed since the
        # last one (grayscale mean abs diff -- this example's own choice
        # of motion signal) and the actual bytes/sec recently sent.
        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        motion = 0.0
        if prev_gray is not None:
            motion = float(np.mean(cv2.absdiff(gray, prev_gray)))
        prev_gray = gray
        quality = controller.next_quality(motion)
        capture_ts = time.time_ns()
        ok, adaptive_jpeg = cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, quality])
        adaptive_bytes = adaptive_jpeg.tobytes()
        controller.record_sent(len(adaptive_bytes))
        node.publish_compressed_image(TOPIC_IMAGE_ADAPTIVE, adaptive_bytes, frame_id,
                                       capture_timestamp_ns=capture_ts, quality=quality)

        print(f"frame {frame_id}: raw={len(raw_bytes)} bytes, compressed={len(jpeg_bytes)} bytes, "
              f"adaptive={len(adaptive_bytes)} bytes (motion={motion:.1f}, quality={quality})")

        node.spin_once()
        frame_id += 1
        time.sleep(0.2)  # ~5 FPS


def run_subscriber(node: RelinkNode, display: bool = False):
    def on_raw(frame_id, data):
        expected = FRAME_WIDTH * FRAME_HEIGHT * 3
        if len(data) != expected:
            print(f"image_raw: frame {frame_id} dropped ({len(data)} bytes, expected {expected})",
                  file=sys.stderr)
            return
        arr = np.frombuffer(data, dtype=np.uint8).reshape((FRAME_HEIGHT, FRAME_WIDTH, 3))
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

    def on_adaptive(frame_id, data, capture_timestamp_ns, quality):
        latency_ms = (time.time_ns() - capture_timestamp_ns) / 1e6
        with open("latest_adaptive.jpg", "wb") as f:
            f.write(data)
        print(f"image_adaptive: frame {frame_id} complete ({len(data)} bytes, quality={quality}, "
              f"latency={latency_ms:.1f}ms) -> latest_adaptive.jpg")

    node.subscribe_image(TOPIC_IMAGE_RAW, on_raw)
    node.subscribe_image(TOPIC_IMAGE_COMPRESSED, on_compressed)
    node.subscribe_compressed_image(TOPIC_IMAGE_ADAPTIVE, on_adaptive)

    if display:
        print("camera_stream: subscribed, showing image_raw live in a cv2 window "
              "(press q or ctrl-c to quit)")
    else:
        print("camera_stream: subscribed, writing latest_raw.jpg / latest_compressed.jpg / "
              "latest_adaptive.jpg to the current directory as frames complete")
    node.spin()


def main():
    if len(sys.argv) < 2:
        print(f"usage: {sys.argv[0]} [pub|sub] [rlcore_ip] [--display] [--video-file PATH]",
              file=sys.stderr)
        return 1

    args = sys.argv[2:]
    display = "--display" in args
    video_file = None
    if "--video-file" in args:
        i = args.index("--video-file")
        if i + 1 >= len(args):
            print("camera_stream: --video-file requires a path", file=sys.stderr)
            return 1
        video_file = args[i + 1]
    rlcore_ip = next((a for a in args if not a.startswith("--") and a != video_file), None)

    node = RelinkNode()
    if rlcore_ip:
        node.set_rlcore.ip(rlcore_ip)
    else:
        node.use_multicast_discovery()

    if sys.argv[1] == "pub":
        run_publisher(node, video_file=video_file)
    else:
        run_subscriber(node, display=display)
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""relink_image_benchmark -- head-to-head throughput/delivery comparison
of raw, uncompressed full-HD image frames sent two ways:

  - udp:  publish_image()/subscribe_image()          (relink/node.py)
  - shm:  publish_local_ipc_image()/subscribe_local_ipc_image()
          (same-host shared-memory ring, relink/shm_transport.py)

A raw 1920x1080 BGR8 frame is ~6.2MB, chunked into ~4,450 UDP
datagrams by publish_image() -- losing any ONE of those chunks drops
the whole frame (no retransmission, see subscribe_image()'s docstring).
The shared-memory ring has no MTU and no chunking at all: a whole frame
is one ring slot. That difference is the entire point of this
benchmark -- see the main README's Step 12 (Benchmarks) and Step 15's
"Same-host shared-memory IPC" deep dive for measured numbers and the
full explanation.

REQUIRES OPENCV, WHICH IS NOT PART OF RELINK AND IS NOT INSTALLED FOR
YOU. Install it yourself first:
    pip install opencv-python

Run as two processes, e.g.:
    python3 examples/relink_image_benchmark.py pub shm
    python3 examples/relink_image_benchmark.py sub shm

    python3 examples/relink_image_benchmark.py pub udp
    python3 examples/relink_image_benchmark.py sub udp

IMPORTANT for `udp`: start the PUBLISHER process first, then the
subscriber a moment later (~0.3-1s is enough). Multicast discovery
sends a one-shot startup burst (3 beacons within ~400ms) and then goes
quiet for 30-60s -- whichever side's listener isn't running yet when
the OTHER side's burst goes out won't learn about it until the next
re-announce, which is well past this benchmark's run time. This
doesn't apply to `shm`: there's no peer to discover, subscribing
attaches to the ring directly.

Defaults to a real webcam (device 0); use --video-file PATH to drive it
from a video file instead (loops at the end), same convention as
camera_stream.py. --duration SEC controls the publisher's run length
(default 8s); the subscriber always listens a couple seconds longer to
catch the tail.
"""
import sys
import os
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from relink import RelinkNode

try:
    import cv2
except ImportError:
    print("relink_image_benchmark.py requires OpenCV -- install it yourself first:\n"
          "    pip install opencv-python", file=sys.stderr)
    sys.exit(1)

TOPIC_UDP = 800
TOPIC_SHM = 801
FRAME_WIDTH = 1920
FRAME_HEIGHT = 1080
EXPECTED_LEN = FRAME_WIDTH * FRAME_HEIGHT * 3


def run_pub(transport: str, duration_sec: float, video_file: str):
    node = RelinkNode()
    shm = transport == "shm"

    if shm:
        if not node.advertise_local_ipc_image(TOPIC_SHM):
            print("advertise_local_ipc_image() failed", file=sys.stderr)
            sys.exit(1)
    else:
        node.use_multicast_discovery()
        node.advertise_image(TOPIC_UDP)
        node.spin_once()

    cap = cv2.VideoCapture(video_file) if video_file else cv2.VideoCapture(0)
    if not cap.isOpened():
        print(f"could not open {video_file or 'camera device 0'}", file=sys.stderr)
        sys.exit(1)

    frame_id = 0
    pushed = 0
    full_drops = 0
    bytes_total = 0
    start = time.monotonic()
    deadline = start + duration_sec
    while time.monotonic() < deadline:
        ok, frame = cap.read()
        if not ok:
            if video_file:
                cap.set(cv2.CAP_PROP_POS_FRAMES, 0)
                continue
            break  # real camera returning nothing is a real failure, not "loop"
        if frame.shape[1] != FRAME_WIDTH or frame.shape[0] != FRAME_HEIGHT:
            frame = cv2.resize(frame, (FRAME_WIDTH, FRAME_HEIGHT))
        payload = frame.tobytes()
        bytes_total += len(payload)
        if shm:
            if node.publish_local_ipc(TOPIC_SHM, payload):
                pushed += 1
            else:
                full_drops += 1
        else:
            node.publish_image(TOPIC_UDP, payload, frame_id)
            node.spin_once()
            pushed += 1
        frame_id += 1

    elapsed = time.monotonic() - start
    if shm:
        node.request_stop()

    print(f"PUB_SUMMARY transport={transport} frames={frame_id} pushed={pushed} "
          f"full_drops={full_drops} elapsed={elapsed:.2f}s fps={pushed/elapsed:.1f} "
          f"throughput_MBps={bytes_total/elapsed/1e6:.1f}")


def run_sub(transport: str, duration_sec: float):
    node = RelinkNode()
    shm = transport == "shm"
    received = 0
    wrong_size = 0
    lock = threading.Lock()

    def on_frame_shm(payload):
        nonlocal received, wrong_size
        with lock:
            received += 1
            if len(payload) != EXPECTED_LEN:
                wrong_size += 1

    def on_frame_udp(frame_id, data):
        nonlocal received, wrong_size
        with lock:
            received += 1
            if len(data) != EXPECTED_LEN:
                wrong_size += 1

    if shm:
        if not node.subscribe_local_ipc_image(TOPIC_SHM, on_frame_shm):
            print("subscribe_local_ipc_image() failed", file=sys.stderr)
            sys.exit(1)
        time.sleep(duration_sec)
        node.request_stop()
    else:
        node.use_multicast_discovery()
        node.subscribe_image(TOPIC_UDP, on_frame_udp)

        def stopper():
            time.sleep(duration_sec)
            node.request_stop()

        threading.Thread(target=stopper, daemon=True).start()
        node.spin()  # subscribe_image() alone never starts the transport thread

    with lock:
        c = received
        w = wrong_size

    print(f"SUB_SUMMARY transport={transport} received={c} wrong_size={w} "
          f"elapsed={duration_sec:.2f}s fps={c/duration_sec:.1f}")


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ("pub", "sub") or sys.argv[2] not in ("udp", "shm"):
        print(f"usage: {sys.argv[0]} <pub|sub> <udp|shm> [--duration SEC] [--video-file PATH]\n\n"
              "Raw full-HD (1920x1080 BGR8) image throughput/delivery benchmark:\n"
              "the UDP-socket transport (publish_image/subscribe_image) vs the\n"
              "same-host shared-memory IPC ring (publish_local_ipc_image/\n"
              "subscribe_local_ipc_image). See the main README's Step 12\n"
              "(Benchmarks) for measured numbers and Step 15's same-host\n"
              "shared-memory IPC deep dive for why they differ this much.\n\n"
              "For --transport udp: start the PUBLISHER process first, then the\n"
              "subscriber ~0.3-1s later -- see this file's module docstring for why.",
              file=sys.stderr)
        return 1

    mode = sys.argv[1]
    transport = sys.argv[2]
    args = sys.argv[3:]
    duration_sec = 8.0
    video_file = None
    if "--duration" in args:
        i = args.index("--duration")
        duration_sec = float(args[i + 1])
    if "--video-file" in args:
        i = args.index("--video-file")
        video_file = args[i + 1]

    if mode == "pub":
        run_pub(transport, duration_sec, video_file)
    else:
        run_sub(transport, duration_sec + 2.0)  # margin to catch the publisher's tail
    return 0


if __name__ == "__main__":
    sys.exit(main())

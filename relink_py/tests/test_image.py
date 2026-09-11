"""Image type tests -- mirrors tests/test_image.cpp."""
import os
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from relink.image import (
    ImageChunk, encode_image_chunks, ImageReassembler,
    IMAGE_CHUNK_DATA_BYTES, MAX_PAYLOAD_BYTES,
)
from relink.node import RelinkNode
import ctypes

failures = 0


def check(cond, desc):
    global failures
    if cond:
        print(f"ok: {desc}")
    else:
        print(f"FAIL: {desc}", file=sys.stderr)
        failures += 1


# --- ImageChunk fills the MTU budget ---
check(ctypes.sizeof(ImageChunk) <= MAX_PAYLOAD_BYTES, "ImageChunk fits in one UDP datagram")

# --- pure encode/reassemble round trip ---
data = bytes(i % 256 for i in range(5000))
chunks = []
encode_image_chunks(42, data, chunks.append)
expected_chunks = (len(data) + IMAGE_CHUNK_DATA_BYTES - 1) // IMAGE_CHUNK_DATA_BYTES
check(len(chunks) == expected_chunks, "encode produced expected chunk count")
check(chunks[0].frame_id == 42, "frame_id set on every chunk")

completed = {}
reassembler = ImageReassembler(lambda fid, img: completed.update(frame_id=fid, image=img))
for c in chunks:
    reassembler.on_chunk(c)
check(completed.get("frame_id") == 42, "reassembly reports correct frame_id")
check(completed.get("image") == data, "reassembled bytes match original")

# --- empty image ---
chunks2 = []
encode_image_chunks(1, b"", chunks2.append)
check(len(chunks2) == 1 and chunks2[0].chunk_bytes == 0, "empty image is one empty chunk")

empty_result = {}
r2 = ImageReassembler(lambda fid, img: empty_result.update(img=img))
r2.on_chunk(chunks2[0])
check(empty_result.get("img") == b"", "empty image reassembles to empty bytes")

# --- dropped chunk never completes; fresh frame_id resets cleanly ---
data3 = bytes([0xAB]) * 5000
chunks3 = []
encode_image_chunks(7, data3, chunks3.append)
check(len(chunks3) >= 3, "large enough to span multiple chunks")

complete_count = [0]
r3 = ImageReassembler(lambda fid, img: complete_count.__setitem__(0, complete_count[0] + 1))
r3.on_chunk(chunks3[0])
for c in chunks3[2:]:  # skip chunks3[1] -- simulates a dropped datagram
    r3.on_chunk(c)
check(complete_count[0] == 0, "incomplete frame never fires on_complete")

data4 = bytes([0xCD]) * 3000
chunks4 = []
encode_image_chunks(8, data4, chunks4.append)
for c in chunks4:
    r3.on_chunk(c)
check(complete_count[0] == 1, "fresh frame_id after a dropped one reassembles cleanly")

# --- real end-to-end over RelinkNode via multicast ---
pub = RelinkNode()
sub = RelinkNode()
pub.use_multicast_discovery()
sub.use_multicast_discovery()

lock = threading.Lock()
result = {}


def on_image(frame_id, img):
    with lock:
        result["frame_id"] = frame_id
        result["image"] = img


sub.subscribe_image(600, on_image)
pub.advertise_image(600)

t1 = threading.Thread(target=sub.spin_once)
t2 = threading.Thread(target=pub.spin_once)
t1.start(); t2.start()
t1.join(); t2.join()

time.sleep(1.5)

big_data = bytes((i * 3) % 256 for i in range(10000))
sent = pub.publish_image(600, big_data)
check(sent, "publish_image reports success")

deadline = time.time() + 5
while time.time() < deadline:
    with lock:
        if "image" in result:
            break
    time.sleep(0.05)

with lock:
    check(result.get("image") == big_data, "real multicast delivery reassembles correctly")

print()
if failures == 0:
    print("ALL PASS")
    sys.exit(0)
else:
    print(f"{failures} FAILURE(S)")
    sys.exit(1)

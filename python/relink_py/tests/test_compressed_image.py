"""CompressedImage type tests -- mirrors tests/test_compressed_image.cpp."""
import ctypes
import os
import random
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from relink.compressed_image import (
    CompressedImageChunk, encode_compressed_image_chunks, CompressedImageReassembler,
    COMPRESSED_IMAGE_CHUNK_DATA_BYTES, COMPRESSED_IMAGE_CHUNK_HEADER_BYTES, MAX_PAYLOAD_BYTES,
)
from relink.node import RelinkNode

failures = 0


def check(cond, desc):
    global failures
    if cond:
        print(f"ok: {desc}")
    else:
        print(f"FAIL: {desc}", file=sys.stderr)
        failures += 1


# --- CompressedImageChunk fills the MTU budget ---
check(ctypes.sizeof(CompressedImageChunk) <= MAX_PAYLOAD_BYTES,
      "CompressedImageChunk fits in one UDP datagram")
check(COMPRESSED_IMAGE_CHUNK_HEADER_BYTES == 19, "header is 19 bytes")

# --- pure encode/reassemble round trip, out of order (UDP gives no
# ordering guarantee -- timestamp/quality live on every chunk so this
# must still work even if chunk 0 isn't first to arrive) ---
data = bytes(i % 256 for i in range(5000))
chunks = []
encode_compressed_image_chunks(42, data, 123456789, 37, chunks.append)
expected_chunks = (len(data) + COMPRESSED_IMAGE_CHUNK_DATA_BYTES - 1) // COMPRESSED_IMAGE_CHUNK_DATA_BYTES
check(len(chunks) == expected_chunks, "encode produced expected chunk count")
check(chunks[0].frame_id == 42, "frame_id set on every chunk")
check(all(c.capture_timestamp_ns == 123456789 and c.quality == 37 for c in chunks),
      "timestamp/quality stamped on every chunk")

shuffled = list(chunks)
random.Random(1234).shuffle(shuffled)

completed = {}
reassembler = CompressedImageReassembler(
    lambda fid, img, ts, q: completed.update(frame_id=fid, image=img, ts=ts, q=q))
for c in shuffled:
    reassembler.on_chunk(c)
check(completed.get("frame_id") == 42, "reassembly reports correct frame_id")
check(completed.get("image") == data, "reassembled bytes match original, even out of order")
check(completed.get("ts") == 123456789, "capture_timestamp_ns survives reassembly")
check(completed.get("q") == 37, "quality survives reassembly")

# --- empty image ---
chunks2 = []
encode_compressed_image_chunks(1, b"", 0, 0, chunks2.append)
check(len(chunks2) == 1 and chunks2[0].chunk_bytes == 0, "empty image is one empty chunk")

empty_result = {}
r2 = CompressedImageReassembler(lambda fid, img, ts, q: empty_result.update(img=img))
r2.on_chunk(chunks2[0])
check(empty_result.get("img") == b"", "empty image reassembles to empty bytes")

# --- dropped chunk never completes; fresh frame_id resets cleanly ---
data3 = bytes([0xAB]) * 5000
chunks3 = []
encode_compressed_image_chunks(7, data3, 111, 50, chunks3.append)
check(len(chunks3) >= 3, "large enough to span multiple chunks")

complete_count = [0]
r3 = CompressedImageReassembler(lambda fid, img, ts, q: complete_count.__setitem__(0, complete_count[0] + 1))
r3.on_chunk(chunks3[0])
for c in chunks3[2:]:  # skip chunks3[1] -- simulates a dropped datagram
    r3.on_chunk(c)
check(complete_count[0] == 0, "incomplete frame never fires on_complete")

data4 = bytes([0xCD]) * 3000
chunks4 = []
encode_compressed_image_chunks(8, data4, 222, 60, chunks4.append)
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


def on_image(frame_id, img, ts, quality):
    with lock:
        result["frame_id"] = frame_id
        result["image"] = img
        result["ts"] = ts
        result["quality"] = quality


sub.subscribe_compressed_image(601, on_image)
pub.advertise_compressed_image(601)

t1 = threading.Thread(target=sub.spin_once)
t2 = threading.Thread(target=pub.spin_once)
t1.start(); t2.start()
t1.join(); t2.join()

time.sleep(1.5)

big_data = bytes((i * 3) % 256 for i in range(10000))
sent = pub.publish_compressed_image(601, big_data, frame_id=99,
                                     capture_timestamp_ns=987654321, quality=72)
check(sent, "publish_compressed_image reports success")

deadline = time.time() + 5
while time.time() < deadline:
    with lock:
        if "image" in result:
            break
    time.sleep(0.05)

with lock:
    check(result.get("image") == big_data, "real multicast delivery reassembles correctly")
    check(result.get("ts") == 987654321, "capture_timestamp_ns delivered end to end")
    check(result.get("quality") == 72, "quality delivered end to end")

print()
if failures == 0:
    print("ALL PASS")
    sys.exit(0)
else:
    print(f"{failures} FAILURE(S)")
    sys.exit(1)

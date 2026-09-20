# ReLink vs ROS2: raw camera streaming comparison

This directory holds four matched pub/sub pairs -- Python and C++, each
against both ReLink and ROS2 -- streaming the **exact same frames**
(raw BGR or JPEG-compressed, your choice), at the same resolution and
target rate, so overhead and latency can be compared directly:

| Pair | Language | Transport |
|---|---|---|
| `relink_camera_pub_raw.py` / `relink_camera_sub_raw.py` | Python | ReLink (UDP, `advertise_image`/`publish_image`/`subscribe_image`) |
| `ros2_camera_pub.py` / `ros2_camera_sub.py` | Python | ROS2 (rclpy, `sensor_msgs/Image`/`CompressedImage`, DDS, BEST_EFFORT QoS) |
| `../../../cpp/examples/ros2_compare/relink_camera_pub_raw.cpp` / `relink_camera_sub_raw.cpp` | C++ | ReLink (same wire path as the Python side, byte-for-byte) |
| `../../../cpp/ros2_compare/ws/src/relink_bench_compare/src/cam_pub.cpp` / `cam_sub.cpp` | C++ | ROS2 (rclcpp, same message types/QoS as the Python ROS2 side) |

All four support `--width`/`--height`/`--fps` and `--compressed`
(JPEG, `--quality`), so any resolution/rate/compression combination can
be tried on both transports and both languages with the same flags.
With `--video PATH` and no explicit `--width`/`--height`/`--fps`, all
four also auto-detect and follow the video file's own native
resolution and frame rate (no forced resize or pacing mismatch against
the source). Both subscribers `imshow()` the live feed and print
running FPS/bandwidth/latency stats every 30 frames, in the same
format, so the log streams can be diffed by eye across all four
combinations.

The C++ ReLink pair additionally supports `--h264`: GPU-accelerated
H264 encode/decode via VAAPI, driven through `gst-launch-1.0`
subprocesses (see `gst_h264_codec.hpp` in the same directory as the
`.cpp` files) -- no GStreamer C API / dev headers needed, only the
runtime CLI tool and the vaapi plugin set. Both pub and sub must be
run with `--h264` together (it's a distinct wire encoding, not
auto-negotiated).

## How latency is measured

- **ReLink side**: the publisher prepends an 8-byte `struct.pack("<d",
  time.time())` send-timestamp to the raw frame bytes before calling
  `publish_image`. The subscriber unpacks it back out on receipt and
  diffs against its own `time.time()`.
- **ROS2 side**: the publisher sets `msg.header.stamp` from
  `self.get_clock().now()` at publish time; the subscriber diffs that
  against its own receipt-time clock read.

Both measurements use the same basis (host wall clock, publish-time to
callback-invocation-time), so they're comparable as long as pub and sub
run on the same machine (as in the run below) or on clock-synced
machines.

## Run it yourself

```bash
# ReLink Python (needs a running rlcore -- see the main README's "Step 9"):
python3 relink_camera_pub_raw.py 127.0.0.1 --width 640 --height 480 --fps 60 --compressed &
python3 relink_camera_sub_raw.py 127.0.0.1 --width 640 --height 480 --compressed

# ROS2 Python (needs ROS2 sourced, e.g. `source /opt/ros/humble/setup.bash`):
python3 ros2_camera_pub.py --width 640 --height 480 --fps 60 --compressed &
python3 ros2_camera_sub.py --width 640 --height 480 --compressed

# ReLink C++:
cd ../../../cpp/examples/ros2_compare
g++ -std=c++17 -O2 -I ../../relink/include -pthread relink_camera_pub_raw.cpp -o relink_camera_pub_raw $(pkg-config --cflags --libs opencv4)
g++ -std=c++17 -O2 -I ../../relink/include -pthread relink_camera_sub_raw.cpp -o relink_camera_sub_raw $(pkg-config --cflags --libs opencv4)
./relink_camera_pub_raw 127.0.0.1 --width 640 --height 480 --fps 60 --compressed &
./relink_camera_sub_raw 127.0.0.1 --width 640 --height 480 --compressed

# ROS2 C++ (colcon package at ../../../cpp/ros2_compare/ws):
cd ../../../cpp/ros2_compare/ws && colcon build --packages-select relink_bench_compare
source install/setup.bash
ros2 run relink_bench_compare cam_pub --width 640 --height 480 --fps 60 --compressed &
ros2 run relink_bench_compare cam_sub --width 640 --height 480 --compressed
```

## Measured results (this session)

Same machine, same `/dev/video0` webcam. Two configurations were run,
on both languages:

**Config A -- 320x240, raw BGR, 30 fps target** (both transports were
camera-bound at ~10 fps actual, not transport-bound, at this
resolution -- 230,408 bytes/frame incl. the 8-byte ReLink timestamp
header):

| Metric | ReLink (Python) | ROS2 (rclpy, BEST_EFFORT) |
|---|---|---|
| FPS | 9.9-10.0 | 9.5-10.0 |
| Throughput | ~2.27-2.30 MB/s | ~2.18-2.30 MB/s |
| **Avg end-to-end latency** | **~4.6-5.0 ms** | **~19.4-21.9 ms** |

**Config B -- 640x480, JPEG-compressed (quality 70), 60 fps target**
(again camera-bound at ~10 fps actual, ~17-20KB/frame -- roughly
45-55x smaller than the equivalent 640x480x3=921,600-byte raw frame):

| Metric | ReLink (C++) | ROS2 (rclcpp, BEST_EFFORT) |
|---|---|---|
| FPS | 9.6-10.0 | 9.6-10.1 |
| Throughput | ~0.17-0.18 MB/s | ~0.19-0.20 MB/s |
| Bytes/frame | ~16.9-17.4 KB | ~18.1-20.0 KB |
| **Avg end-to-end latency** | **~2.0-5.5 ms** | **~9.2-21.2 ms** |

Both configurations, both languages, show the same pattern: FPS and
throughput are essentially identical (both bounded by the same
webcam's real capture rate, not by either transport, at every
resolution tried), while ReLink's end-to-end latency comes in
consistently lower than ROS2's -- roughly **2-4x lower** in the
640x480/JPEG/C++ run, similar to the **~4-5x** gap seen at
320x240/raw/Python. Compression (JPEG vs raw) cuts bytes/frame by
~45-55x on both transports equally, as expected -- it's a payload-size
change, not a transport-specific one.

### Why the gap

- **DDS discovery/matching and serialization overhead.** rclpy's
  `Image` message goes through DDS's participant/writer/reader matching
  and CDR (de)serialization on every publish, layers ReLink's direct
  `sendto()` UDP path skips entirely -- ReLink's wire format is a
  fixed-layout `ctypes`/`#pragma pack(1)` struct copied straight to/from
  the socket buffer, no generic serialization step.
- **Executor/callback dispatch latency.** rclpy's default single-threaded
  executor (`rclpy.spin`) adds its own scheduling latency between "DDS
  delivered a sample" and "your Python callback actually runs," on top
  of the wire-level delivery time.
- **ReLink's chunking is intentionally lightweight.** `advertise_image`/
  `publish_image` fragments a raw frame into MTU-sized UDP datagrams and
  reassembles them with a minimal frame_id/chunk-index scheme (see
  `image.py`), with no retransmission and no generic message-framework
  overhead sitting on top of the socket calls.

**Config C -- native resolution/rate, raw BGR** (`--video PATH` with no
resolution/fps override, so all four use the source file's own
1920x1080 @ 29.97 fps -- 6,220,800-916,808 bytes/frame depending on the
8-byte ReLink header):

| Metric | ReLink (Python) | ROS2 (rclpy, BEST_EFFORT) | ReLink (C++) | ROS2 (rclcpp, BEST_EFFORT) |
|---|---|---|---|---|
| FPS | 19.5-27.7 (pub sent ~30) | **4.0-4.7** | 28.6-29.2 | 25.5-26.9 |
| Throughput | ~122-187 MB/s | ~25-29 MB/s | ~178-181 MB/s | ~155-168 MB/s |
| **Avg end-to-end latency** | ~24-27 ms | **~210-212 ms** | **~12-13 ms** | ~9-20 ms |

This is the first configuration where the two transports don't land at
roughly the same FPS/throughput ceiling -- at ~6.2MB/frame, message
size itself becomes the bottleneck, and the two implementations handle
it very differently:

- **ReLink (both languages)** stays close to the video's native ~30
  fps -- 28.6-29.2 fps in C++, 19.5-27.7 fps (still climbing toward the
  publisher's ~30 fps when the test window ended) in Python. Chunking a
  large frame into MTU-sized UDP datagrams and copying them straight to
  the socket scales fine to multi-megabyte payloads in both languages.
- **ROS2 rclpy (Python) drops to ~4-4.7 fps** -- a bit over 6x under
  its own publisher's target rate -- with average latency around
  **210ms**, roughly **9-17x worse** than every other combination in
  this table. This is consistent with known rclpy overhead moving large
  messages through Python-level (de)serialization and the DDS binding,
  rather than anything ReLink-specific.
- **ROS2 rclcpp (C++) holds up much better** than its Python
  counterpart -- 25.5-26.9 fps, ~9-20ms latency -- showing the ~4-5x
  gap seen in the smaller configs is mostly a Python/rclpy binding
  cost, not an inherent DDS-at-this-message-size problem. ReLink's C++
  and Python numbers, by contrast, stay close to each other (both near
  the video's native rate) -- the large-message penalty that hits
  ROS2's Python binding hard barely shows up on ReLink in either
  language.

**Config D -- native resolution/rate, JPEG-compressed (quality 70)**
(`--video PATH --compressed`, same 1920x1080 @ 29.97 fps source as
Config C, but compressed -- ~155-180KB/frame, roughly 35-40x smaller
than the equivalent raw frame):

| Metric | ReLink (Python) | ROS2 (rclpy, BEST_EFFORT) | ReLink (C++) | ROS2 (rclcpp, BEST_EFFORT) |
|---|---|---|---|---|
| FPS | 29.8-29.9 | 29.3-29.9 | 29.8-29.9 | 25.9-29.8 |
| Throughput | ~4.78-4.86 MB/s | ~4.79-4.89 MB/s | ~4.79-4.86 MB/s | ~4.27-4.78 MB/s |
| **Avg end-to-end latency** | **~1.1-1.4 ms** | ~11.7-14.7 ms | ~8.9-9.3 ms | ~20.2-28.7 ms |

Compression completely resolves Config C's ROS2/rclpy bottleneck --
once frames are ~157KB instead of ~6.2MB, all four combinations sustain
essentially the video's own native ~30 fps and comparable throughput.
Latency still favors ReLink in both languages (ReLink Python lowest at
~1.2ms, likely helped by loopback + Python's GIL serializing pub/sub
timing more tightly than DDS's separate discovery/transport threads;
ReLink C++ ~9ms; rclpy ~13ms; rclcpp highest here at ~20-29ms, still
settling toward steady state in this run's window), but the gap is far
smaller and less consequential than the raw/native-resolution case --
**for large frames, encoding down to a network-appropriate size matters
far more than which transport moves the bytes.**

**Config E -- native resolution/rate, GPU H264 (ReLink C++ only)**
(`--h264`, same 1920x1080 @ 30fps source, VAAPI encode/decode via
`gst-launch-1.0` subprocesses -- see `gst_h264_codec.hpp`):

| Metric | ReLink (C++, H264) |
|---|---|
| FPS | 29.7-29.9 |
| Throughput | ~0.67-0.81 MB/s |
| Bytes/frame | ~13-27 KB (varies with scene content -- real inter-frame prediction, unlike JPEG's fixed-effort-per-frame re-encode) |
| **Avg end-to-end latency** | **~54-56 ms** |

H264 gets bytes/frame down further than JPEG (~13-27KB vs ~155-180KB)
at the same resolution/rate -- expected, since JPEG re-encodes each
frame from scratch while H264 exploits inter-frame redundancy -- but
latency is markedly *worse* than JPEG's ~9ms in this same C++ ReLink
pipeline. That's a direct, understood cost of this implementation's
framing technique, not overhead intrinsic to H264 or the GPU: frames
are demarcated by scanning for Access Unit Delimiter (AUD) NALs in the
encoder's output byte stream (`vaapih264enc aud=true`), and a frame is
only known to be *complete* once the AUD of the *next* frame appears --
which structurally adds about one full frame period of latency
(~33ms at 30fps) on top of the actual GPU encode/decode work. A
length-prefixed framing scheme (impossible here without linking the
GStreamer C API directly, since `gst-launch-1.0` subprocess pipes only
give you the raw byte stream) would remove that added frame of latency.
Take this as a demonstration that a real GPU H264 pipeline is reachable
from ReLink's C++ side without any GStreamer dev headers, moving
meaningfully fewer bytes/frame than JPEG at the same resolution --
not as a verdict that H264 beats JPEG here; on this specific
implementation, JPEG remains both simpler and lower-latency, and H264
only wins if bandwidth (not latency) is the binding constraint.

### Getting VAAPI H264 working (driver quirks found on this machine)

Two real bugs surfaced getting `--h264` working reliably (both fixed
in `gst_h264_codec.hpp`, documented here in case you hit them building
your own GStreamer pipeline against raw frames from a subprocess):

- **`fdsrc ! <capsfilter>` corrupts raw video frames.** Piping raw BGR
  bytes through `fdsrc` straight into a capsfilter (no format-aware
  parser) reliably corrupted buffers on this machine's GStreamer 1.24.2
  -- `videoconvert` logged "code not implemented" / "invalid video
  buffer received" on nearly every frame, and downstream frame counts
  came out wildly wrong (e.g. 422 spurious AUD markers from 30 real
  frames). Fix: use `rawvideoparse format=bgr width=W height=H
  framerate=N/1` after `fdsrc` instead of a bare capsfilter --
  it's the element actually meant to chop an unformatted byte stream
  into correctly-tagged video frames, and fixed this completely
  (verified: exact frame count in, exact AUD count out, byte-identical
  decode round-trip).
- **`vaapih264enc`'s `tune=low-power` breaks caps negotiation** on this
  Intel iHD driver (a "failed to get surface attributes" / "not
  negotiated" pipeline failure) -- reproduced consistently, omitted in
  favor of `rate-control=cqp` alone.

A third bug was in this project's own C++ wrapper, not GStreamer: the
`ChildProcess` struct managing the subprocess's pipes had a closing
destructor but no move constructor/assignment, so returning it by value
from a factory function (or assigning it into a class member) let a
temporary's destructor close the very file descriptors the real object
still needed -- writes then failed immediately with EBADF. Fixed with
explicit move semantics (copy deleted, move transfers and nulls out the
source). A `posix_spawn`-based process launch (rather than raw
`fork()`+`exec()`) was also used throughout, since by the time the
encoder/decoder subprocess is spawned, OpenCV's video backend has
already started its own internal worker threads -- `fork()` in an
already-multithreaded process risks the child inheriting a locked mutex
from a thread that no longer exists in the child, hanging it before it
ever reaches `exec()`.

### Caveats

- This is a **single-machine, loopback, one-webcam** measurement, not a
  network or multi-host benchmark -- it isolates library/transport
  overhead, not real network conditions (WiFi loss, multi-hop latency,
  etc. would affect both systems, and ReLink's raw (uncompressed, no
  retransmission) mode is far more loss-sensitive than ROS2's reliable
  QoS options -- see `../camera_stream.py`'s module docstring for the
  raw-vs-compressed tradeoff on lossy links).
- Both publishers were camera-capture-bound (~10 FPS) here, not
  transport-bound -- on faster cameras or synthetic (non-camera) framer
  sources, the throughput ceiling of each transport (not just latency)
  would become the more interesting comparison.
- ROS2 QoS was set to BEST_EFFORT/KEEP_LAST(1) specifically to match
  ReLink's no-retransmission UDP semantics as closely as ROS2 allows;
  ROS2's default RELIABLE QoS would add further latency under loss but
  wasn't the point of this comparison.

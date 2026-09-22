# ReLink -- Python binding

Independent Python re-implementation of ReLink's wire protocol, stdlib
only (`ctypes` + `socket` + `struct`), not a wrapper around the C++
library (`relink/`). Proven wire-compatible with the C++ node in both
directions and over both discovery modes -- see `tests/two_process_pub.py`
/ `tests/two_process_sub.py`.

New here? Run this now, in two terminals:

```bash
python3 examples/hello_relink.py   # run it again in a second terminal
```

Two copies find each other over multicast and start exchanging
messages within a second or two — no daemon, no config, no manual IP
address. See the root `README.md`'s **Zero to one** walkthrough for
what's happening and why.

## Abstract -- why this exists

Robotics and distributed-sensor stacks have reached for ROS as the
default pub/sub layer for over a decade, but ROS's messaging core
carries costs that show up directly in latency and jitter budgets:
ROS1's TCPROS pays a per-message connection-management tax and depends
on a single master for topic registration; ROS2's DDS fixes the master
but replaces it with continuous multicast re-announcement (SPDP) that
scales as O(N²) with node count and can flood constrained or WiFi
networks the longer a system stays up. Neither was designed around a
hard real-time latency floor -- they were designed around generality
(arbitrary QoS policies, arbitrary transports, arbitrary serialization),
and generality has a cost.

**ReLink exists for the case where you don't need that generality and do
need the latency floor**: a fixed-layout message over raw UDP,
discovered once at startup (not re-announced forever), serialized by a
straight `memcpy`/`ctypes` cast instead of a schema-driven encoder,
dispatched on a dedicated thread with no lock in the hot path. In
head-to-head measurement against ROS2 Humble on identical
hardware/payload/rate (see the root `README.md`'s Benchmarks section),
that design difference is not theoretical: ReLink's C++ node measured
**~3x lower average latency and ~10x lower worst-case tail latency**.

This Python binding exists so that role isn't C++-only: a Python node
speaks the exact same bytes on the wire as the C++ one (proven
bidirectionally, over both discovery modes, in `tests/two_process_*.py`)
-- useful for tooling, test scripts, ground-station/UI processes, or any
node that isn't itself on the hard real-time hot path but still needs to
publish or subscribe into a ReLink system. It is a from-scratch,
stdlib-only reimplementation of the documented byte layout, not a
C-extension wrapper around the C++ library, so it carries zero
third-party dependencies and stays honest about not claiming the C++
side's 1000Hz hot-path guarantee (see Performance note below): Python's
interpreter overhead and per-object heap allocation make it unsuitable
for the same zero-allocation hot-path claim the C++ transport makes, so
this binding is positioned for interoperability and non-hot-path nodes,
not as a second implementation racing the C++ core for the performance
target.

### Performance note

`relink/include/relink/udp_transport.hpp`'s docstring states this
explicitly: the C++ side's 1000Hz/≤1ms hard requirement is a claim about
that implementation specifically, not the protocol in the abstract.
Every language binding gets the same wire format and discovery modes;
only C++ (with CPU pinning + `SCHED_FIFO`, see the root README's
Benchmarks) has been measured against the hard latency floor.

The tradeoff is explicit, not hidden: ReLink is a **protocol**, not a
general messaging framework. One message type per topic, fixed at
registration. No arbitrary QoS matrix, no reliable-transport path in v1,
no schema evolution story. If your system needs those things, ROS2/DDS
is the more complete answer. If your system needs a small, fixed set of
nodes on a LAN exchanging fixed-layout messages with minimal overhead --
and Python is the right language for that particular node -- this
binding is built specifically for that case.

## Install

No build step -- it's pure Python stdlib:

```
cd relink_py
python3 -c "import relink"   # or add relink_py/ to PYTHONPATH
```

## Quick start

```python
import ctypes
from relink import RelinkNode, Float32

class ImuReading(ctypes.Structure):
    _pack_ = 1
    _fields_ = [("accel_x", ctypes.c_float), ("accel_y", ctypes.c_float), ("accel_z", ctypes.c_float)]

node = RelinkNode()
node.set_rlcore.ip("10.0.0.5")          # mode A, or node.use_multicast_discovery() for mode B

node.advertise(100, ImuReading)
node.publish(100, ImuReading(accel_x=0.1, accel_y=0.2, accel_z=9.81))

node.subscribe(101, Float32, lambda msg: print(msg.data))
node.spin()
```

Custom types are plain `ctypes.Structure` subclasses with `_pack_ = 1` --
the Python analog of C++'s `#pragma pack(1)` + `std::is_trivially_copyable`
contract. No registration, no schema exchange: any conforming struct just
works.

## Examples

See the root `README.md`'s **All examples** table for what each one
shows; the Python versions live here:

- `examples/hello_relink.py` -- start here: one file, no arguments, run
  it twice and watch two copies find each other and exchange messages
- `examples/rlcore_pubsub.py` -- mode A (daemon), custom + default type
- `examples/multicast_pubsub.py` -- mode B (no daemon)
- `examples/camera_stream.py` -- a real webcam (or `--video-file PATH`,
  for testing without one) streamed three ways: raw, fixed-quality
  JPEG, and adaptive-quality JPEG via `CompressedImage` +
  `AdaptiveBitrateController` (`relink/compressed_image.py` /
  `relink/adaptive_bitrate.py`), demonstrating why you'd chunk,
  compress, and adapt quality for a large message; requires OpenCV,
  which you install yourself (`pip install opencv-python`) -- it is not
  a ReLink dependency
- `examples/relink_image_benchmark.py` -- raw full-HD image throughput
  and delivery rate, UDP socket vs same-host shared-memory IPC, side by
  side; also requires OpenCV -- see **Same-host IPC** below

Run the pub/sub examples as two processes: `python3 rlcore_pubsub.py
pub <rlcore_ip>` and `python3 rlcore_pubsub.py sub <rlcore_ip>`.

## Same-host IPC

If publisher and subscriber are two processes on the same machine,
`advertise_local_ipc`/`subscribe_local_ipc`/`publish_local_ipc` (and
the `_image` variants for whole `Image`/`CompressedImage` frames) go
through a shared-memory ring instead of UDP -- no socket, no chunk-loss
mode for raw frames. See the root `README.md`'s [same-host
shared-memory IPC](../../README.md#same-host-shared-memory-ipc-_local_ipc)
deep-dive section for the API, constraints, and measured numbers, and
run `examples/relink_image_benchmark.py pub/sub udp` vs `pub/sub shm`
to reproduce them yourself (see the file's module docstring for a UDP
launch-order caveat).

Want to poke at a `*_local_ipc` topic without writing code? `rltopic`'s
`hz`/`bw`/`echo`/`pub` subcommands take an `--ipc` flag that targets
the shared-memory ring directly instead of the network -- see the root
`python/README.md`'s "Using the commands" section for examples.

## Tests

```
cd relink_py
python3 tests/test_relink.py
```

Plain-assert suite mirroring the C++ `tests/test_*.cpp` coverage: wire
layout, frame codec, register/beacon codec, real-socket UDP delivery,
RelinkNode's discovery-mode error paths, `Image`/`CompressedImage`
chunking + reassembly (`tests/test_image.py` /
`tests/test_compressed_image.py`), and `AdaptiveBitrateController`'s
motion/bitrate quality selection (`tests/test_adaptive_bitrate.py`).

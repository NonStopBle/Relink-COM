# ReLink

A lightweight, ROS-like publish/subscribe protocol over UDP — built to be
faster and simpler than ROS1/ROS2, with reference implementations in
**C++** and **Python** that talk the same wire protocol interchangeably.

## Abstract — why this exists

Robotics and distributed-sensor stacks have reached for ROS as the
default pub/sub layer for over a decade, but ROS's messaging core carries
costs that show up directly in latency and jitter budgets: ROS1's
TCPROS pays a per-message connection-management tax and depends on a
single master for topic registration; ROS2's DDS fixes the master but
replaces it with continuous multicast re-announcement (SPDP) that scales
as O(N²) with node count and can flood constrained or WiFi networks the
longer a system stays up. Neither was designed around a hard real-time
latency floor — they were designed around generality (arbitrary QoS
policies, arbitrary transports, arbitrary serialization), and generality
has a cost.

**ReLink exists for the case where you don't need that generality and do
need the latency floor**: a fixed-layout message over raw UDP, discovered
once at startup (not re-announced forever), serialized by a straight
`memcpy` instead of a schema-driven encoder, dispatched on a dedicated
thread with no lock in the hot path. In head-to-head measurement against
ROS2 Humble on identical hardware/payload/rate (see Benchmarks below),
that design difference is not theoretical: ReLink measured **~3x lower
average latency and ~10x lower worst-case tail latency**.

The tradeoff is explicit, not hidden: ReLink is a **protocol**, not a
general messaging framework. One message type per topic, fixed at
registration. No arbitrary QoS matrix, no reliable-transport path in v1,
no schema evolution story. If your system needs those things, ROS2/DDS
is the more complete answer. If your system needs a `1000Hz+`,
sub-millisecond, small-message control loop between a known set of nodes
on a LAN — sensor fusion, joint-state streaming, a control loop between
a planner and a driver — ReLink is built specifically for that case and
gives up generality to get there.

**Byte layout over library**: every struct on the wire (header, beacon,
default types, custom types) is documented as an exact byte layout, not
just a C++ struct — which is *why* a second language binding (this repo
ships both C++ and Python, proven wire-compatible in both directions) is
a from-scratch reimplementation of the same bytes, not a wrapper around
the C++ library. Any language that can open a UDP socket and pack a
fixed struct can speak ReLink.

## Zero to one

The fastest way to see it work: run the same tiny program twice (two
terminals, or two computers on the same network) and watch them find
each other and start talking, with no daemon, no config file, and no
manual IP address to type in.

| Step | Do this | Why |
|---|---|---|
| **1** | `git clone https://github.com/NonStopBle/Relink-COM.git && cd Relink-COM` | Get the code. |
| **2** | Pick a language: **C++** or **Python**. Both talk the exact same protocol, so it doesn't matter which — you can even mix them. | ReLink isn't tied to one language. |
| **3** | **C++**: `g++ -std=c++17 -I relink/include -pthread examples/cpp/hello_relink.cpp -o hello_relink`<br>**Python**: nothing to build — it's plain stdlib. | C++ needs a compile step; Python doesn't. |
| **4** | Open **two terminal windows**. In each one, run the same program:<br>**C++**: `./hello_relink`<br>**Python**: `python3 relink_py/examples/hello_relink.py` | Each copy is both a sender and a receiver. |
| **5** | Watch both windows. Within a second or two, each one prints `sent: N (to 1 peer(s))` and `received: N` — they found each other automatically over the network (multicast) and are exchanging a counter, once a second, in both directions. | This is discovery + pub/sub working, live. |
| **6** | Open `relink_py/examples/hello_relink.py` or `examples/cpp/hello_relink.cpp` and read it — it's under 60 lines. Change `TOPIC_HELLO`, the message type, or what happens when a message arrives, and you're writing your own ReLink node. | The whole API surface is: `advertise`, `subscribe`, `publish`, `spin`. |

If step 5 never prints `received:` on either side, the two copies most
likely aren't on the same network segment, or something on the network
is blocking UDP multicast (some WiFi routers and most cloud VPCs do) —
see **Discovery modes** below for the alternative (a small daemon
instead of multicast) that works around that.

## Features

- **UDP by default**, fixed 7-byte header, `'#' ... '\n'` framed, no
  scanning for delimiters inside the payload.
- **std_msgs-style default types** (`Bool`, `Int32`, `Float32`, ...) plus
  user-defined custom types — any trivially-copyable struct (C++
  `#pragma pack(1)` / Python `ctypes.Structure` with `_pack_ = 1`) just
  works, no registration, no schema exchange.
- **Two mutually-exclusive discovery modes**, chosen explicitly per node:
  - **Mode A — `relink-com-core`**: a small central daemon (C++ and
    Python builds, byte-identical wire behavior), request/ACK
    registration, retry-with-backoff.
  - **Mode B — multicast beacon**: fully decentralized, no daemon,
    jittered startup burst + sparse re-announce (not continuous —
    ReLink's traffic doesn't grow with uptime the way ROS2's does).
- **UDP NAT traversal** (`relink-com-core --nat`): the daemon acts as a
  rendezvous point, handing out each node's real (NAT-mapped) endpoint
  instead of its private LAN address, paired with client-side hole
  punching — lets nodes across separate networks/NATs find each other.
- **Built-in `Image` type** (`advertise_image` / `publish_image` /
  `subscribe_image`) for messages bigger than one UDP datagram —
  automatically chunks to the MTU maximum on send and reassembles on
  receive, no hand-rolled chunking needed (see `camera_stream` in All
  examples below).
- **Dedicated data thread**, zero heap allocation and zero locking in the
  benchmarked hot path, with opt-in CPU pinning and `SCHED_FIFO` for
  real-time tail-latency control.
- **C++ and Python bindings**, proven bidirectionally wire-compatible —
  a C++ publisher and a Python subscriber (or vice versa) talk normally,
  over either discovery mode.

## Discovery modes — which one do I use?

Every node picks exactly one, explicitly:

```cpp
// Mode A: a small daemon both sides can reach. Works everywhere,
// including networks that block multicast (most WiFi, most cloud VPCs).
node.set_com_core.ip("10.0.0.5");

// Mode B: no daemon needed, nodes find each other via multicast.
// Simplest to start with, but needs a network that allows multicast.
node.use_multicast_discovery();
```

Same choice in Python: `node.set_com_core.ip(...)` or
`node.use_multicast_discovery()`.

If you're not sure which to use: try mode B (`hello_relink`, above)
first since it needs nothing extra to run. If it doesn't work on your
network, switch to mode A — start the daemon once (`./relink-com-core`
or `python3 com-core/relink_com_core.py`), then point every node at its
IP address.

## All examples

Every example below is a complete, runnable program — not a snippet.
Each one exists in both C++ (`examples/cpp/`) and Python
(`relink_py/examples/`).

| Example | What it shows | Run it |
|---|---|---|
| **`hello_relink`** | The simplest possible ReLink program. One file, no arguments, runs the same way on both ends — each copy is both a publisher and a subscriber, looping forever, sending a counter once a second. Start here. | `./hello_relink` / `python3 hello_relink.py` (run twice) |
| **`comcore_pubsub`** | Mode A (daemon) discovery, a custom message type (`ImuReading`, several `float`s + a timestamp) alongside a default type (`Float32`), one process as publisher and one as subscriber. | `./comcore_pubsub pub <daemon_ip>` and `... sub <daemon_ip>` in separate terminals, with the daemon already running |
| **`multicast_pubsub`** | The same pub/sub shape as `comcore_pubsub`, but mode B (no daemon) — shows the two discovery modes are interchangeable from the application's point of view. | `./multicast_pubsub pub` and `... sub` |
| **`camera_stream`** | A real webcam streamed over ReLink two ways at once, using the built-in `Image` type (`advertise_image`/`publish_image`/`subscribe_image` — see Features below) — `image_raw` (uncompressed, hundreds of MTU-sized chunks per frame) and `image_compressed` (JPEG, 2-3 chunks per frame). In real testing, C++ delivered raw frames ~96-98% reliably and compressed ~100%; Python delivered compressed ~100% but raw effectively 0% — Python's per-chunk interpreter overhead can't keep up with a several-hundred-chunk burst the way C++ can, so **always compress before sending from Python**. **Requires OpenCV, which you install yourself** (`pip install opencv-python`, or `sudo apt install libopencv-dev` for C++) — it is not a ReLink dependency. | `./camera_stream pub` and `... sub` |

There's also a performance test harness (`relink_benchmark.cpp` at the
repo root) used to produce the numbers in the Benchmarks section below —
worth reading once you're comfortable with the basics, not a starting
point.

## Benchmarks

Measured over 60s sustained at 1000Hz with a ~40-byte payload
(`ImuReading`-sized), with CPU pinning + `SCHED_FIFO` applied to the data
thread:

| Metric | ReLink (C++) | ROS2 Humble (default QoS) |
|---|---|---|
| avg latency | 169 µs | 504 µs |
| p99 latency | 297 µs | 989 µs |
| **worst-case latency** | **388 µs** | 3703 µs |
| sustained rate | ~5900 Hz | ~1985 Hz |
| 1ms budget | **PASS** | FAIL |

Baseline (no CPU pinning/`SCHED_FIFO`) worst-case was ~2.9ms — pinning
the data thread to a dedicated core and giving it real-time scheduling
priority is what gets it under budget. These numbers were measured on
loopback on a single dev machine, not real wired LAN with two physical
nodes — directionally strong, not a certified LAN result.

## Repository layout

```
relink/include/relink/     C++ library (header-only)
  wire.hpp                   byte-exact structs: RelinkHeader, BeaconPacket, default types
  ring_buffer.hpp            fixed-capacity, drop-oldest-on-overflow
  frame.hpp                  pure encode/decode, MTU-budgeted
  udp_transport.hpp          dedicated data thread, CPU pinning, SCHED_FIFO
  register.hpp / com_core_client.hpp   mode A (com-core) client
  beacon.hpp / multicast_discovery.hpp mode B (multicast) client
  relink.hpp                  RelinkNode -- the public API

com-core/
  relink_com_core.cpp        registration daemon, C++ build
  relink_com_core.py         registration daemon, Python build (byte-identical protocol)

relink_py/relink/          Python library (stdlib-only: ctypes + socket + struct)
  (mirrors the C++ layer-for-layer, see relink_py/README.md)

examples/cpp/                C++ usage examples (see "All examples" above)
relink_py/examples/          Python usage examples (see "All examples" above)
tests/, relink_py/tests/     unit tests + two-process correctness tests, both languages
ros2_compare/                 ROS2 Humble comparison benchmark package
```

## Quick start — C++

```cpp
#include "relink/relink.hpp"

RelinkNode node;
node.set_com_core.ip("10.0.0.5");        // mode A; or node.use_multicast_discovery() for mode B

node.advertise<Float32>(100);
node.publish<Float32>(100, Float32{ .data = 36.6f });

node.subscribe<Float32>(101, [](const Float32& msg) { /* ... */ });
node.spin();
```

Build against `relink/include/` (header-only). See `examples/cpp/` above
for full publisher/subscriber programs.

## Quick start — Python

```python
from relink import RelinkNode, Float32

node = RelinkNode()
node.set_com_core.ip("10.0.0.5")         # mode A; or node.use_multicast_discovery() for mode B

node.advertise(100, Float32)
node.publish(100, Float32(data=36.6))

node.subscribe(101, Float32, lambda msg: print(msg.data))
node.spin()
```

No install step — pure stdlib. See `relink_py/README.md` and
`relink_py/examples/` above for full programs, including custom
`ctypes.Structure` message types.

## Running com-core (mode A discovery daemon)

```
# C++
./relink-com-core --port 8445 [--nat]

# Python
python3 com-core/relink_com_core.py --port 8445 [--nat]
```

Either build works with either language's nodes. `--nat` enables UDP
NAT traversal (see Features above) for nodes on separate networks.

## Testing

```
# C++ (each test is a standalone binary)
g++ -std=c++17 -I relink/include -pthread tests/test_wire.cpp -o test_wire && ./test_wire

# Python
python3 relink_py/tests/test_relink.py
```

Every layer (wire format, ring buffer, framing, discovery, the public
API's error paths) has a plain-assert test suite in both languages, plus
two-process correctness tests and cross-language interop tests
(`tests/two_process_{pub,sub}.cpp` / `relink_py/tests/two_process_{pub,sub}.py`)
proving the C++ and Python nodes actually interoperate over real
sockets, not just in theory.

## Status

Core is complete and tested: wire format, ring buffer, UDP transport,
both discovery modes, the public API, correctness tests, the 1000Hz
benchmark (passing with CPU pinning + `SCHED_FIFO`), a ROS2 comparison,
discovery stress tests (boot storm, power-cycle, simulated network
drop), a Python binding, and NAT traversal.

**Explicitly out of scope for now**: TCP reliable transport, AES-256-GCM
encryption (`secure=true`, API shape decided but unimplemented), message
fragmentation beyond one UDP datagram's MTU budget (see `camera_stream`
above for the recommended workaround pattern), and automatic
multi-language codegen (bindings are hand-written per language,
deliberately).

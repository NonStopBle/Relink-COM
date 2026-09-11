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
- **Dedicated data thread**, zero heap allocation and zero locking in the
  benchmarked hot path, with opt-in CPU pinning and `SCHED_FIFO` for
  real-time tail-latency control.
- **C++ and Python bindings**, proven bidirectionally wire-compatible —
  a C++ publisher and a Python subscriber (or vice versa) talk normally,
  over either discovery mode.

## Performance

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

Baseline (no CPU pinning/`SCHED_FIFO`) worst-case was ~2.9ms — the
optimizations above are what get it under budget; see
`relink-com-spec.md`'s Lean-optimization section for the full technique
list and ordering. These numbers were measured on loopback on a single
dev machine, not real wired LAN with two physical nodes, which is the
spec's actual hard-pass condition — directionally strong, not a
certified LAN result.

## Repository layout

```
relink-com-spec.md         full wire-format + design spec (source of truth)
relink_com_implement.md    build-order summary

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

examples/cpp/               C++ usage examples
relink_py/examples/         Python usage examples
tests/, relink_py/tests/    unit tests + two-process correctness tests, both languages
ros2_compare/                ROS2 Humble comparison benchmark package
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

Build against `relink/include/` (header-only). See `examples/cpp/` and
the root `relink_example.cpp` / `relink_benchmark.cpp` for full
publisher/subscriber programs.

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
`relink_py/examples/` for full programs, including custom
`ctypes.Structure` message types.

## Running com-core

```
# C++
./relink-com-core --port 8445 [--nat]

# Python
python3 com-core/relink_com_core.py --port 8445 [--nat]
```

Either build works with either language's nodes — that's the point.

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

v1 core (steps 1-11 of the build order) complete: wire format, ring
buffer, UDP transport, both discovery modes, the public API, correctness
tests, the 1000Hz benchmark (passing with CPU pinning + `SCHED_FIFO`),
a ROS2 comparison, discovery stress tests (boot storm, power-cycle,
simulated network drop), a Python binding, and NAT traversal.

**Explicitly out of scope for v1** (see `relink-com-spec.md`'s deferred
list): TCP reliable transport, AES-256-GCM encryption (`secure=true`,
API shape decided but unimplemented), message fragmentation beyond one
UDP datagram's MTU budget, and automatic multi-language codegen (bindings
are hand-written per language, deliberately).

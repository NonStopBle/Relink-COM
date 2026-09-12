<div align="center">

# ReLink

**A lightweight, ROS-like publish/subscribe protocol over UDP — built to be
faster and simpler than ROS1/ROS2.**

![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)
![Python](https://img.shields.io/badge/Python-3.8%2B-blue)
![Transport](https://img.shields.io/badge/transport-UDP-orange)
![Platform](https://img.shields.io/badge/platform-Linux-lightgrey)

You do not need ROS installed. You do not need a master or a DDS
implementation. Two reference implementations — **C++** and **Python** —
speak the exact same bytes on the wire and are interchangeable.

</div>

---

## Table of contents

- [Step 0 — What ReLink actually is](#step-0--what-relink-actually-is)
- [Step 1 — Get the code](#step-1--get-the-code)
- [Step 2 — Zero to one (run it)](#step-2--zero-to-one-run-it)
- [Step 3 — Pick a discovery mode](#step-3--pick-a-discovery-mode)
- [Step 4 — Named topics](#step-4--named-topics)
- [Step 5 — Quick start: C++](#step-5--quick-start-c)
- [Step 6 — Quick start: Python](#step-6--quick-start-python)
- [Step 7 — Message types](#step-7--message-types)
- [Step 8 — Sending images](#step-8--sending-images)
- [Step 9 — Running the rlcore daemon](#step-9--running-the-rlcore-daemon)
- [Step 10 — All examples](#step-10--all-examples)
- [Step 11 — Testing](#step-11--testing)
- [Step 12 — Benchmarks](#step-12--benchmarks)
- [Step 13 — NAT traversal (cross-network nodes)](#step-13--nat-traversal-cross-network-nodes)
- [Step 14 — Troubleshooting](#step-14--troubleshooting)
- [Reference](#reference)
  - [Feature matrix](#feature-matrix)
  - [Repository layout](#repository-layout)
  - [Status / scope](#status--scope)

---

## Step 0 — What ReLink actually is

| Word | What it actually means |
|---|---|
| **Node** | Any process using `RelinkNode` (C++) or `RelinkNode` (Python). No node type distinction — every node can advertise, subscribe, and publish at once. |
| **Topic** | A named or numbered channel. One message type per topic, fixed at registration — not renegotiable later. |
| **Discovery** | How nodes find each other's IP/port before they can exchange messages. ReLink has exactly two modes (Step 3) — no auto-negotiated QoS, no DDS-style SPDP. |
| **Wire format** | The literal byte layout of every packet — documented, not just implied by a struct definition. A from-scratch reimplementation in any language that can open a UDP socket can speak ReLink. |
| **rlcore** | The optional small daemon used for Mode A discovery. Not required — Mode B (multicast) needs no daemon at all. |

> **Why this exists:** ROS1's TCPROS pays a per-message connection-management
> tax and depends on a single master. ROS2's DDS fixes the master but
> replaces it with continuous multicast re-announcement (SPDP) that scales
> as O(N²) with node count and grows the longer a system stays up. Neither
> was designed around a hard real-time latency floor — both were designed
> around generality (arbitrary QoS, arbitrary transports, arbitrary
> serialization), and generality has a cost. ReLink gives up that
> generality — one message type per topic, no reliable-transport path in
> v1, no schema evolution — in exchange for a fixed-layout message over
> raw UDP, discovered once (not re-announced forever), serialized by a
> straight `memcpy`, dispatched on a dedicated thread with no lock in the
> hot path. Measured head-to-head against ROS2 Humble on identical
> hardware/payload/rate (Step 12): **~3x lower average latency, ~10x lower
> worst-case tail latency**.

If your system needs TCP reliability, arbitrary QoS policies, or schema
evolution, ROS2/DDS is the more complete answer. If your system needs a
`1000Hz+`, sub-millisecond, small-message control loop between a known set
of nodes on a LAN — sensor fusion, joint-state streaming, a control loop
between a planner and a driver — this is built specifically for that case.

---

## Step 1 — Get the code

```bash
git clone https://github.com/NonStopBle/Relink-COM.git
cd Relink-COM
```

This is currently the only way to install it — there is no package
manager entry, on purpose (see Step 0: no codegen, no dependency surface
beyond a socket).

---

## Step 2 — Zero to one (run it)

The fastest way to see it work: run the same tiny program twice (two
terminals, or two computers on the same network) and watch them find each
other and start talking — no daemon, no config file, no manual IP address.

| | C++ | Python |
|---|---|---|
| **Build** | `g++ -std=c++17 -I relink/include -pthread examples/cpp/hello_relink.cpp -o hello_relink` | nothing to build — pure stdlib |
| **Run (in two terminals)** | `./hello_relink` | `python3 relink_py/examples/hello_relink.py` |

Within a second or two, each window prints `sent: N (to 1 peer(s))` and
`received: N` — the two copies found each other over multicast and are
exchanging a counter, once a second, in both directions.

**Check it worked**: if step 2 never prints `received:` on either side,
see [Step 14 — Troubleshooting](#step-14--troubleshooting).

**Why it worked**: `hello_relink.{cpp,py}` is under 60 lines. Open it and
read it — the whole API surface is `advertise`, `subscribe`, `publish`,
`spin`. Change `TOPIC_HELLO`, the message type, or what happens on
receive, and you're writing your own ReLink node.

---

## Step 3 — Pick a discovery mode

Every node picks exactly one, explicitly — there's no auto-detection,
because silently falling back to a different discovery mechanism is
exactly the kind of hidden behavior ReLink is trying to avoid.

```cpp
// Mode A: a small daemon both sides can reach. Works everywhere,
// including networks that block multicast (most WiFi, most cloud VPCs).
node.set_rlcore.ip("10.0.0.5");

// Mode B: no daemon needed, nodes find each other via multicast.
// Simplest to start with, but needs a network that allows multicast.
node.use_multicast_discovery();
```

```python
# Same choice in Python:
node.set_rlcore.ip("10.0.0.5")
# or:
node.use_multicast_discovery()
```

| Mode | Needs a daemon? | Works over WiFi/cloud VPC (multicast usually blocked)? | Use when |
|---|---|---|---|
| **A — rlcore** | Yes (`relink-rlcore`) | Yes | Your network blocks multicast, or you want one known rendezvous point |
| **B — multicast beacon** | No | No | Simplest LAN setup, nothing extra to run |

Not sure which to use? Try Mode B first (Step 2 already did). If it
doesn't work on your network, switch to Mode A — see
[Step 9](#step-9--running-the-rlcore-daemon).

Either mode, a node shares one UDP socket across every topic by default
(demultiplexed by topic id). Call `node.set_multiplex(false)` for ROS's
one-port-per-topic model instead — see
[Step 12's benchmarks](#step-12--benchmarks) for the throughput/latency
tradeoff before reaching for it.

`rl_topic` (`rl_topic.py`/`rl_topic.cpp`, a `rostopic`-style CLI —
`list`/`info`/`hz`/`bw`/`echo`/`pub`) needs no special flag or code
change to work against a node running `set_multiplex(false)`: a
demultiplexed node's beacon/registration already announces each topic's
real per-topic port, so `rl_topic`'s own node (which stays on the
default shared socket) discovers and talks to it exactly like any other
peer. Verified against a `set_multiplex(false)` publisher: `list`
discovers the topic, `hz`/`echo` correctly read it, and `pub` correctly
reaches a `set_multiplex(false)` subscriber. The only failure mode
encountered was the ordinary beacon-startup-timing race (see Step 14),
unrelated to multiplex mode.

---

## Step 4 — Named topics

Topics can be a human-readable string instead of a hand-assigned number:

```cpp
node.advertise<Float32>("/relink/temperature");
```

```python
node.advertise("/relink/temperature", Float32)
```

**How it works**: ReLink hashes the string (FNV-1a) down to the `uint32_t`
that actually goes on the wire — both sides just need to type the same
string, no shared header/constant needed. This is a **one-way hash, not a
reversible encoding**: packing an arbitrary-length name losslessly into 4
bytes isn't possible past ~6 characters. Each node keeps a local name
registry (`topic_name_for()` / `rltopic_list()`), built only from its own
calls, for debugging — not a network-wide directory. A genuine hash
collision, or a name landing on the reserved NAT-punch id, throws loudly
instead of silently misrouting.

A numeric id still works exactly as before, if you'd rather assign ids by
hand:

```cpp
node.subscribe<Float32>(101, [](const Float32& msg) { /* ... */ });
```

> **Performance note — resolve the string once, outside the hot loop.**
> Every call to the string overload (`publish<T>(name, ...)`,
> `advertise<T>(name, ...)`, `subscribe<T>(name, ...)`) re-hashes the
> string (FNV-1a over every character), takes the node's internal lock,
> and does a registry lookup+comparison — every single call, not just the
> first. That's fine for `advertise`/`subscribe` (called once at startup),
> but calling the *string* overload of `publish()` inside a tight publish
> loop pays that cost on every message. Resolve the name to its numeric
> id once with `topic_id_for()` (C++) / `_topic_id_for()` (Python) —
> idempotent, always returns the same id for the same name — and call the
> numeric overload of `publish()` in the loop instead:
>
> ```cpp
> uint32_t topic_id = node.topic_id_for("/relink/temperature"); // once
> for (...) {
>     node.publish<Float32>(topic_id, Float32{ .data = reading }); // hot loop: numeric, no re-hash
> }
> ```
>
> Measured effect (microbenchmark, no peers attached, isolating just the
> resolution cost): the numeric overload costs **~26 ns/call**, the string
> overload **~49 ns/call** — the FNV-1a hash + lock + registry lookup
> roughly **doubles** per-call overhead versus a bare numeric id. At the
> multi-hundred-kHz burst rates in Step 12, that difference is exactly
> the kind of per-message tax that determines whether the sender or
> receiver becomes the bottleneck first — resolve once, publish by id.

---

## Step 5 — Quick start: C++

```cpp
#include "relink/relink.hpp"

RelinkNode node;
node.set_rlcore.ip("10.0.0.5");        // mode A; or node.use_multicast_discovery() for mode B

// Topics can be a name (hashed to a wire id for you) or a hand-assigned number:
node.advertise<Float32>("/relink/temperature");
node.publish<Float32>("/relink/temperature", Float32{ .data = 36.6f });

node.subscribe<Float32>(101, [](const Float32& msg) { /* ... */ });
node.spin();
```

Build against `relink/include/` — header-only, no linking step beyond
`-pthread`. See `examples/cpp/` (Step 10) for full publisher/subscriber
programs.

---

## Step 6 — Quick start: Python

```python
from relink import RelinkNode, Float32

node = RelinkNode()
node.set_rlcore.ip("10.0.0.5")         # mode A; or node.use_multicast_discovery() for mode B

# Topics can be a name (hashed to a wire id for you) or a hand-assigned number:
node.advertise("/relink/temperature", Float32)
node.publish("/relink/temperature", Float32(data=36.6))

node.subscribe(101, Float32, lambda msg: print(msg.data))
node.spin()
```

No install step — pure stdlib (`ctypes` + `socket` + `struct`). See
`relink_py/README.md` and `relink_py/examples/` (Step 10) for full
programs, including custom `ctypes.Structure` message types.

---

## Step 7 — Message types

`std_msgs`-style default types are built in (`Bool`, `Int32`, `Float32`,
...), plus **user-defined custom types**: any trivially-copyable struct
just works, no registration, no schema exchange, no codegen step.

```cpp
#pragma pack(push, 1)
struct ImuReading { float ax, ay, az; uint64_t timestamp_us; };
#pragma pack(pop)

node.advertise<ImuReading>("/relink/imu");
```

```python
import ctypes

class ImuReading(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [("ax", ctypes.c_float), ("ay", ctypes.c_float),
                ("az", ctypes.c_float), ("timestamp_us", ctypes.c_uint64)]

node.advertise("/relink/imu", ImuReading)
```

Both sides must independently define the identical byte layout — ReLink
does no schema negotiation between nodes, same as ROS relies on both
sides being built against the same generated message header.

---

## Step 8 — Sending images

Messages bigger than one UDP datagram use the built-in `Image` type:

```cpp
node.advertise_image("/relink/camera/front");
node.publish_image("/relink/camera/front", jpeg_bytes, jpeg_len);
node.subscribe_image("/relink/camera/front", [](uint32_t frame_id, const uint8_t* data, size_t len) { /* ... */ });
```

It automatically chunks to the MTU maximum on send and reassembles on
receive — no hand-rolled chunking needed (see `camera_stream` in Step 10).

> **Not a free win.** `advertise_image`/`subscribe_image` request a larger
> (1MB) socket buffer so a several-hundred-chunk burst has room to sit in
> the kernel queue instead of overflowing and silently dropping the tail
> of the image. That buffer only helps with **short bursts a fast
> subscriber will drain in time** — it trades drops for latency, not a
> free win. If your subscriber callback is slower than the publish rate
> (measured: a 50ms/frame JPEG-decode-and-process callback against a
> faster sender), per-frame latency grows linearly and delivery
> eventually collapses once the backlog exceeds the buffer regardless of
> buffer size. Since callbacks run inline on ReLink's one data thread, the
> actual fix for a slow consumer is to keep the callback fast — hand heavy
> work (decoding, disk I/O, ML inference) off to your own worker
> thread/queue instead of doing it inside the callback. Prefer
> `image_compressed`-style payloads over raw frames for anything
> real-time: a dropped chunk drops the whole image (no retransmission),
> and fewer chunks per frame means fewer chances to drop one.

---

## Step 9 — Running the rlcore daemon

Only needed for Mode A discovery (Step 3):

```bash
# C++
./relink-rlcore --port 8445 [--nat]

# Python
python3 rlcore/relink_rlcore.py --port 8445 [--nat]
```

Either build works with either language's nodes — same wire protocol.
`--nat` enables UDP NAT traversal (Step 13).

---

## Step 10 — All examples

Every example is a complete, runnable program, not a snippet — each one
exists in both C++ (`examples/cpp/`) and Python (`relink_py/examples/`).

| Example | What it shows | Run it |
|---|---|---|
| **`hello_relink`** | The simplest possible node — one file, no arguments, both publisher and subscriber at once. Start here. | `./hello_relink` / `python3 hello_relink.py` (run twice) |
| **`talker` + `listener`** | Publisher and subscriber split into two separate files/roles, the classic ROS-tutorial shape. | `./listener` then `./talker` |
| **`pubsub`** | `talker` + `listener` combined into one file/process — both roles at once, interoperable with either standalone binary above. | `./pubsub` (run twice, or against `talker`/`listener`) |
| **`rlcore_pubsub`** | Mode A (daemon) discovery, a custom message type alongside a default type, one process as publisher and one as subscriber. | `./rlcore_pubsub pub <daemon_ip>` and `... sub <daemon_ip>`, daemon already running |
| **`multicast_pubsub`** | Same pub/sub shape as `rlcore_pubsub`, but Mode B — shows the two discovery modes are interchangeable from the application's point of view. | `./multicast_pubsub pub` and `... sub` |
| **`camera_stream`** | A real webcam streamed over ReLink two ways at once (`image_raw`, `image_compressed`) using the built-in `Image` type. **Requires OpenCV**, installed yourself — not a ReLink dependency. | `./camera_stream pub` and `... sub` |

There's also a performance test harness (`relink_benchmark.cpp` at the
repo root) used to produce the numbers in Step 12 — worth reading once
you're comfortable with the basics, not a starting point.

---

## Step 11 — Testing

```bash
# C++ (each test is a standalone binary)
g++ -std=c++17 -I relink/include -pthread tests/test_wire.cpp -o test_wire && ./test_wire

# Python
python3 relink_py/tests/test_relink.py
```

Every layer (wire format, ring buffer, framing, discovery, named-topic
hashing, the public API's error paths) has a plain-assert test suite in
both languages, plus two-process correctness tests and cross-language
interop tests (`tests/two_process_{pub,sub}.cpp` /
`relink_py/tests/two_process_{pub,sub}.py`) proving the C++ and Python
nodes actually interoperate over real sockets, not just in theory.

---

## Step 12 — Benchmarks

Measured over 60s sustained at 1000Hz with a ~40-byte payload
(`ImuReading`-sized), CPU pinning + `SCHED_FIFO` applied to the data
thread, on loopback on a single dev machine (directionally strong, not a
certified LAN result):

| Metric | ReLink (C++) | ROS2 Humble (default QoS) |
|---|---|---|
| avg latency | 169 µs | 504 µs |
| p99 latency | 297 µs | 989 µs |
| **worst-case latency** | **388 µs** | 3703 µs |
| 1ms budget | **PASS** | FAIL |

Baseline (no CPU pinning/`SCHED_FIFO`) worst-case was ~2.9ms — pinning the
data thread to a dedicated core and giving it real-time scheduling
priority is what gets it under budget.

**Throughput / jitter, both languages, named-topic pub+sub in one file**
(loopback, `pubsub.{cpp,py}` pattern, paced send):

| Rate | C++ loss | C++ mean latency | Python loss | Python mean latency |
|---|---|---|---|---|
| 1 kHz | 0% | ~32 µs | 0% | ~75 µs |
| 10 kHz | 0% | ~15 µs | 0% | ~47 µs |
| 50 kHz | 0% | ~11 µs | 0% | ~26 µs |
| 100 kHz | ~0.04% | ~9 µs | not sustainable | — |
| unpaced burst | ~500k msg/s, small loss | sub-10 µs | ~90-100k msg/s, ~13% loss | 700 µs-1ms (queueing) |

**Python's ceiling is real, not a bug**: past roughly 50kHz sustained,
CPython's per-message interpreter overhead can't drain the receive socket
fast enough, and a larger socket buffer (Step 8's `enable_large_buffers`)
trades that loss for multi-millisecond queueing latency instead — not a
fix. For sustained rates above that, use the C++ binding.

**Multiplexed (default) vs. per-topic-port (`set_multiplex(false)`)** — a
node normally shares one UDP socket across every topic, demultiplexed by
topic id (see Step 3). `set_multiplex(false)` opts a node into ROS's
one-port-per-topic model instead, for tooling/firewall-rule
compatibility. Measured with 10 topics on one node, both discovery modes:

| Scenario | Mode | Achieved Hz | Loss | Mean latency | Max latency |
|---|---|---|---|---|---|
| C++, multicast, paced | multiplex (default) | ~18.0-18.5k/s | 0% | 18-48 µs | 343-419 µs |
| C++, multicast, paced | per-topic-port | ~17.2-17.3k/s | 0% | 28-35 µs | 2.7-2.9 ms |
| C++, rlcore, paced | multiplex (default) | 18.5k/s | 0% | 27.9 µs | 1.0 ms |
| C++, rlcore, paced | per-topic-port | 14.6k/s | 0% | 35.3 µs | 1.4 ms |
| Python, rlcore, paced | multiplex (default) | 12.8k/s | 0% | 58.4 µs | 0.72 ms |
| Python, rlcore, paced | per-topic-port | 7.2k/s | 0% | 108.7 µs | 2.0 ms |

Single-topic unpaced bursts (200k C++ msgs / 50k Python msgs) showed the
same ordering: multiplex matched or beat per-topic-port on throughput in
every run, in both languages.

**Why**: each extra topic in per-topic-port mode is another OS socket and
another recv thread competing for CPU scheduling — that cost scales with
topic count. The shared-socket default instead pays one topic-id lookup
in a hash map per message (~26ns), which doesn't. `set_multiplex(false)`
is a compatibility feature (ROS-style per-topic addressing), not a
performance one — keep the default unless you specifically need a topic
on its own port.

---

## Step 13 — NAT traversal (cross-network nodes)

`relink-rlcore --nat` makes the daemon act as a rendezvous point: it hands
out each node's real (NAT-mapped) public endpoint instead of its private
LAN address, paired with client-side hole punching, so nodes on separate
networks/NATs can find each other. Requires Mode A (a daemon both sides
can reach) — Mode B's multicast has no path across separate networks by
definition.

**Works transparently with `set_multiplex(false)`.** Each topic's punch
burst fires from that topic's own dedicated socket, not a shared one —
required because a per-topic socket has its own independent NAT mapping,
so punching from the wrong socket would open the wrong mapping and the
peer's simultaneous punch back would never get through. No extra
configuration: a node auto-routes each newly discovered peer's punch
through whichever transport (shared, in the default multiplexed mode, or
that topic's dedicated one, under `set_multiplex(false)`) actually owns
that topic, for both discovery modes — including peers discovered well
after startup, since Mode B's multicast discovery keeps running for the
node's whole lifetime.

**Optional background re-punch.** By default a peer is punched once,
right when it's first learned. `node.enable_nat_repunch(interval_seconds
= 5.0)` instead keeps re-punching every known peer on a timer for the
node's whole lifetime — call it before `spin()`/`publish()` traffic.
This fixes a real but narrow class of failure: a marginal NAT whose
mapping expires faster than expected, or a peer discovered on one side
just before the other side's mapping timed out. It does **not** fix a
NAT/firewall that structurally drops all unsolicited inbound UDP
regardless of timing — verified against a real mobile-carrier NAT with a
raw (non-ReLink) hole-punching test: 85 retry packets over 22 seconds
still delivered zero packets in that direction, and a packet capture on
the receiving host's own network interface confirmed the inbound side's
NAT was dropping them before they ever arrived (not a local firewall —
`ufw` was inactive throughout). No amount of client-side retrying opens
a path that was never open; that case needs a relay/TURN-style fallback,
which ReLink does not currently implement.

## Step 14 — Troubleshooting

| What you see | What it means | Fix |
|---|---|---|
| `hello_relink` never prints `received:` on either side | The two copies aren't on the same network segment, or something is blocking UDP multicast (some WiFi routers, most cloud VPCs) | Switch to Mode A (Step 3/9): run `relink-rlcore` once, point both nodes at its IP with `set_rlcore.ip(...)` |
| `rlcore IP not set` (thrown at `spin()`/`publish()`) | Called `set_rlcore.ip(...)` was never reached, or Mode A was selected without setting an IP | Call `node.set_rlcore.ip("x.x.x.x")` before any traffic, or switch to `use_multicast_discovery()` |
| `no discovery method configured` | Neither discovery mode was selected before `spin()`/`publish()`/`subscribe()` traffic started | Pick exactly one mode (Step 3) before sending/receiving |
| `topic hash collision between "X" and "Y"` | Two different topic names hashed (FNV-1a) to the same 32-bit id — astronomically rare, but checked for | Rename one of the topics |
| High packet loss at a high send rate | The receiver (especially Python) can't drain the socket as fast as it's being filled | Reduce the rate, or move that node to C++ (Step 12) — a bigger socket buffer only postpones this, see Step 8 |
| A dropped/corrupted image frame | `Image` chunks have no retransmission — one lost UDP datagram drops the whole frame | Prefer a compressed payload (`image_compressed`) over raw frames (Step 8), and keep the subscriber callback fast |
| `rl_topic` reports "no peers" right after starting a fresh node/tool pair | Multicast beacons only burst 3x in the first ~400ms, then go silent for 30-60s — starting the two sides even slightly apart can miss that window entirely | Start both sides together, or pass a longer `--timeout` to `rl_topic` so it catches the next sparse re-announce |

---

## Reference

### Feature matrix

| Feature | C++ | Python |
|---|---|---|
| Named topics (string → hashed id) | ✅ | ✅ |
| Default `std_msgs`-style types | ✅ | ✅ |
| Custom trivially-copyable types | ✅ | ✅ |
| Mode A discovery (rlcore) | ✅ | ✅ |
| Mode B discovery (multicast) | ✅ | ✅ |
| UDP NAT traversal | ✅ | ✅ |
| `Image` type (chunk + reassemble) | ✅ | ✅ |
| Zero-copy image chunk send/recv | ✅ (`sendmsg`) | ✅ (`socket.sendmsg`) |
| CPU pinning / `SCHED_FIFO` data thread | ✅ | — (not applicable to CPython's threading model) |
| TCP reliable transport | ❌ (out of scope) | ❌ (out of scope) |
| Encryption (`secure=true`) | ❌ (API shape decided, unimplemented) | ❌ (API shape decided, unimplemented) |

### Repository layout

```
relink/include/relink/     C++ library (header-only)
  wire.hpp                   byte-exact structs: RelinkHeader, BeaconPacket, default types
  topic_hash.hpp              FNV-1a 32-bit hash for named topics
  ring_buffer.hpp             fixed-capacity, drop-oldest-on-overflow
  frame.hpp                   pure encode/decode, MTU-budgeted
  udp_transport.hpp           dedicated data thread, CPU pinning, SCHED_FIFO
  register.hpp / rlcore_client.hpp     mode A (rlcore) client
  beacon.hpp / multicast_discovery.hpp  mode B (multicast) client
  relink.hpp                  RelinkNode -- the public API

rlcore/
  relink_rlcore.cpp          registration daemon, C++ build
  relink_rlcore.py           registration daemon, Python build (byte-identical protocol)

relink_py/relink/           Python library (stdlib-only: ctypes + socket + struct)
  (mirrors the C++ layer-for-layer, see relink_py/README.md)

examples/cpp/                C++ usage examples (Step 10)
relink_py/examples/          Python usage examples (Step 10)
tests/, relink_py/tests/     unit tests + two-process correctness tests, both languages
ros2_compare/                 ROS2 Humble comparison benchmark package
```

### Status / scope

Core is complete and tested: wire format, named-topic hashing, ring
buffer, UDP transport, both discovery modes, the public API, correctness
tests, the 1000Hz benchmark (passing with CPU pinning + `SCHED_FIFO`), a
ROS2 comparison, discovery stress tests (boot storm, power-cycle,
simulated network drop), a Python binding, and NAT traversal.

**Explicitly out of scope for now**: TCP reliable transport, AES-256-GCM
encryption (`secure=true`, API shape decided but unimplemented), message
fragmentation beyond one UDP datagram's MTU budget (see Step 8's
`Image` type for the recommended workaround pattern), and automatic
multi-language codegen (bindings are hand-written per language,
deliberately).

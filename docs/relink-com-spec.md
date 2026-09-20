# ReLink-com — v1 lightweight spec (C++ first, protocol-agnostic by design)

## Goal

ReLink is fundamentally **a wire protocol**, not a C++-only library — the
C++ implementation here is the first reference implementation, not the
whole of what ReLink is. Any language that can open a UDP socket and pack/
unpack a fixed-layout struct can implement a compatible node, because the
byte layout (headers, beacon format, type encoding) is specified
independently of any single language's type system.

This is a deliberate design constraint, not an afterthought: **every
decision in this spec must be expressible as plain bytes on the wire**, not
as a C++-specific mechanism. Concretely:
- `std::is_trivially_copyable` and `#pragma pack` are **C++'s way of
  satisfying** a language-independent rule ("fixed, predictable byte
  layout, no padding") — the rule itself is the protocol requirement;
  other languages satisfy the same rule with their own tools (Python
  `struct.pack`/`ctypes.Structure`, Rust `#[repr(C, packed)]`, etc).
- Topic IDs are plain `uint16_t` numbers on the wire, not language-specific
  enums — any language can send/receive the same topic ID.
- The default std_msgs-style types (`Int32`, `Float32`, etc.) and the
  `MultiArray<T>` layout must be documented as **exact byte layouts**
  (field order, size, endianness), so a Python or Rust implementation can
  reproduce them byte-for-byte without needing the C++ headers.

A lightweight, ROS-like pub/sub protocol, optimized to be faster and
simpler than ROS/ROS2, built on raw UDP by default (TCP optional later).
v1 scope is intentionally minimal — validate the core idea on real hardware
before adding anything else. **v1 ships one reference implementation in
C++ first**, but the protocol itself must not be designed in a way that
would make a second-language implementation structurally harder later.

**Non-goals for v1** (explicitly deferred, do not implement yet):
- No TCP reliable transport path
- No automatic multi-language **codegen** pipeline — hand-written bindings
  per language are fine and expected for v1; only the *automatic
  generation* of bindings from a schema is deferred, not multi-language
  support itself (see Multi-language support section below)
- No external raw-socket / third-party client support beyond what
  documenting the wire format already enables
- No auto-growing buffers — fixed-size ring buffer only

## Multi-language support (protocol-level, not codegen)

Because ReLink is a protocol, adding a second language binding (e.g.
Python) later should mean **re-implementing the same documented byte
layout in that language**, not modifying the C++ library or waiting for a
codegen tool. To keep this true:

- Every struct in this spec (`RelinkHeader`, `BeaconPacket`, default
  types, `MultiArray<T>`) must have its exact byte layout written down —
  field name, type, size in bytes, order, and endianness — as prose or a
  table, not just as a C++ struct definition. The C++ struct is one
  legitimate rendering of that layout, not the definition of it.
- A future Python binding, for example, would use
  `struct.pack("<HHH", topic_id, seq_num, payload_len)` to build the exact
  same `RelinkHeader` bytes the C++ `#pragma pack(1)` struct produces —
  this only works if the byte layout was pinned down precisely enough to
  be language-independent, which is why endianness and exact field sizes
  must be explicit everywhere in this spec, not left as "whatever the
  compiler does."
- Custom user-defined types are inherently per-language already (a user
  writes their own struct in whichever language they're using) — so
  supporting a new language for custom types is just "give users in that
  language the same rules" (trivially-copyable equivalent, fixed packing,
  one type per topic), not new library work.
- When a second language binding is eventually built, add it as a
  separate, independent implementation of this same spec — not as a
  wrapper around the C++ library (e.g. not Python-calls-C++-via-FFI),
  since that would tie every language to C++'s runtime and defeat the
  "any language, any socket" protocol goal.

### Lightweight constraint applies across every language, not just C++

Being a protocol must not become an excuse to add weight in the name of
portability. The whole point of a fixed byte-layout protocol is that it
needs **no heavy serialization framework** in any language:

- **No Protobuf, FlatBuffers, Cap'n Proto, MessagePack, JSON, or any
  schema/codegen serialization library** in any language binding — these
  add parsing overhead, external dependencies, and often heap allocation,
  all of which work against both the lightweight goal and the 1000Hz
  latency budget. A fixed struct + `struct.pack`/`memcpy`-equivalent is
  strictly simpler and faster than any general-purpose serialization
  library, because the layout is fixed and known ahead of time — you're
  not paying for generality you don't need.
- **Each language binding should have zero required third-party
  dependencies** beyond the language's own standard library (sockets +
  binary packing are standard-library features in essentially every
  language: `socket`/`struct` in Python, `<sys/socket.h>` in C++,
  `std::net`/`byteorder`-free manual packing in Rust, etc).
- **A binding in a new language should be small** — realistically a few
  hundred lines: socket setup, header pack/unpack, beacon send/listen,
  a fixed-size ring buffer or even a simple fixed-size array for v1. If a
  new language binding starts requiring its own framework or abstraction
  layer to "fit," that's a sign the protocol itself has grown too complex,
  not a reason to add tooling.
- This directly reinforces the "lightweight" decision made earlier in this
  spec (no TCP, no auto-growing buffers, no codegen, com-core stays out
  of the per-message data path even though it exists for discovery) —
  multi-language support must be achieved by the protocol staying simple
  enough to hand-implement anywhere, not by tooling that papers over a
  complex one.

## Performance target (hard requirement)

On a LAN (wired or WiFi, same subnet), ReLink must sustain **1000 Hz
minimum** publish/receive rate on a single topic between two nodes —
i.e. a full publish → send → recv → deserialize → callback round trip
budget of **≤ 1 ms per message**, sustained, not just a best-case single
message.

This constrains earlier design choices concretely:
- **No per-message heap allocation** anywhere in the send/receive hot
  path — the ring buffer must be pre-allocated (already the plan), and
  the socket send/recv path must reuse fixed buffers, not `new`/`malloc`
  per message.
- **No per-message discovery or connection-setup cost** — this is already
  satisfied by design (discovery resolves peer address once, then it's
  pure UDP send after), but must be verified under benchmark, not assumed.
- **No blocking syscalls in the hot path that could stall past the ~1ms
  budget** — use non-blocking or a tight-timeout recv loop, not a
  blocking `recvfrom` with no timeout, so the loop can service multiple
  topics without one slow read starving the others.
- **Fixed-size struct memcpy only** (per the custom-type rules above) —
  no per-message serialization/reflection cost, since that's the main
  thing that would blow the 1ms budget at 1000Hz.
- This must be measured with real payloads representative of actual use
  (e.g. an `ImuReading`-sized struct, tens of bytes, not a trivial 1-byte
  message) — small messages are easy to hit 1000Hz with; validate with a
  realistic size.
- Guaranteed target applies to **wired LAN**. WiFi is best-effort only —
  do not chase a hard 1000Hz guarantee over WiFi; jitter from retries and
  contention makes that an unrealistic promise. Benchmark both, but only
  wired LAN needs to pass the hard requirement.

**1000Hz is the floor, not the ceiling — C++ should comfortably exceed it.**
This target was set as a conservative safety margin, not a stretch goal.
Raw UDP with a properly tuned C++ hot path (templated zero-cost dispatch,
no GC, direct `memcpy` serialization, and the syscall/threading
techniques in the Lean optimization section below — `recvmmsg`, `epoll`,
CPU pinning, lock-free SPSC ring buffer) realistically reaches
**5,000-10,000Hz+** for small messages on typical modern hardware over
wired LAN. If the actual step-8 benchmark comes in only marginally above
1000Hz rather than in that range, treat it as a signal something in the
hot path is underperforming (an unexpected allocation, a lock, a blocking
call) — profile against the Lean optimization checklist rather than
assuming 1000Hz was always going to be close to the practical limit.

## Lean optimization techniques (apply once basic version works)

These are concrete, well-established techniques for squeezing latency and
jitter out of a UDP hot path — apply them *after* the basic v1 works
correctly (step 7 below), not before. Optimizing an unproven design wastes
effort; optimizing a working one is straightforward.

- **Batch syscalls with `recvmmsg`/`sendmmsg`** instead of one
  `recvfrom`/`sendto` per message. If multiple messages arrive close
  together, one `recvmmsg` call amortizes syscall overhead across all of
  them — meaningful at 1000Hz+ where syscall overhead itself becomes a
  measurable fraction of the 1ms budget.
- **Use `epoll` (edge-triggered) instead of blocking or polling `select`**
  for the recv loop when servicing multiple sockets/topics — avoids
  per-socket polling overhead and scales better as topic count grows.
- **Pre-touch / pre-fault the ring buffer memory at startup** (write to
  every page once before use) so the first real messages don't pay a
  page-fault cost — cheap to do once, avoids an unpredictable first-use
  latency spike.
- **Set `SO_REUSEPORT` / appropriately sized socket buffers**
  (`SO_RCVBUF`/`SO_SNDBUF`) explicitly rather than relying on OS defaults,
  which can be too small for sustained 1000Hz bursts and cause silent
  packet drops under load rather than a clean error.
- **Pin the send/recv thread to a dedicated CPU core** (`sched_setaffinity`
  on Linux) and consider a real-time scheduling class (`SCHED_FIFO`) with
  care — reduces jitter from the OS scheduler moving the thread around or
  preempting it for unrelated work. This is the single highest-leverage
  fix for tail-latency spikes (p99/worst-case), more than raw throughput.
- **Avoid any lock contention in the hot path.** Use a lock-free
  single-producer/single-consumer ring buffer per topic (a well-known,
  simple pattern) rather than a mutex-guarded queue — a single mutex can
  turn into unpredictable latency under contention, which is exactly what
  breaks a 1ms budget's tail.
- **Disable unnecessary logging/debug output in the hot path** — even a
  disabled log statement can cost cycles if not compiled out; use a
  compile-time flag (`#ifdef RELINK_DEBUG`) so release builds have zero
  logging overhead in the send/recv path.
- **Keep the struct small and cache-line aware** where practical — a
  payload that fits in one or two cache lines copies faster than one that
  spans several, though this matters far less than the syscall/threading
  optimizations above for typical small robotics messages.
- **Measure before and after each optimization individually.** Apply one
  change at a time and re-run the benchmark — stacking multiple
  optimizations before measuring makes it impossible to tell which one
  actually helped, and some (like `SCHED_FIFO`) can make things worse if
  misconfigured (starving other necessary system threads).

## Threading and core allocation

The Lean optimization section above mentions pinning "the" send/recv
thread to a core, but doesn't yet define the thread architecture itself.
This section fixes that — how many threads a node should run, what each
does, and how they map to cores, so CPU pinning has something concrete to
pin.

**Thread-per-responsibility, not one thread doing everything:**

- **Data thread** (the hot path): owns the UDP data socket, does
  send/recv, memcpy into/out of ring buffers, invokes subscriber
  callbacks. This is the thread the 1000Hz budget applies to, and the one
  that gets CPU-pinned and considered for `SCHED_FIFO`.
- **Discovery thread**: owns the beacon socket (mode B) or the com-core
  registration socket (mode A). Runs far less often (once at startup,
  then every 30-60s) — must **never share a thread with the data path**,
  since a discovery send/recv blocking briefly must not stall an
  in-flight 1000Hz data message. Does not need pinning or elevated
  scheduling priority — it's cold-path work.
- **User callback execution**: for v1, keep subscriber callbacks running
  **on the data thread itself**, not dispatched to a separate callback
  thread pool. A thread pool adds scheduling/handoff latency (queueing,
  context switch, possible lock) that works directly against the 1ms
  budget. This does mean **user callbacks must be fast and non-blocking**
  — document this as a hard rule for users: a callback that blocks (I/O,
  sleep, heavy computation) stalls the data thread and every other topic
  serviced by it. If a user needs slow processing, they should hand data
  off to their own worker thread from inside the callback, not do the
  slow work inline.

**Core allocation guidance:**

- **Minimum viable**: 2 threads (data + discovery) is enough for a single
  ReLink node handling one or many topics — do not spawn a thread per
  topic, since that multiplies context-switching overhead for no benefit
  (the data thread can service many topics' sockets via one `epoll` loop,
  per the Lean optimization section).
- **Pin the data thread to one dedicated core**, and — if the target
  hardware has cores to spare — leave at least one other core free for
  the OS and discovery thread, so the data thread's core is not
  contended by anything else. On a constrained embedded board with few
  cores, pinning still helps (reduces migration jitter) even without a
  fully free core to dedicate.
- **Do not oversubscribe cores.** If a robot runs multiple ReLink nodes
  on the same board (e.g. one process per subsystem), be deliberate about
  which core each node's data thread pins to — two data threads fighting
  for the same pinned core reintroduces the exact scheduling jitter
  pinning was meant to eliminate. On a 4-core embedded board running 3
  ReLink nodes, pin each to a distinct core and leave one for the OS,
  rather than pinning all three to core 0.
- **`SCHED_FIFO` is per-thread, not per-process** — apply it only to the
  data thread, never to the discovery thread or the whole process. A
  misconfigured discovery thread running `SCHED_FIFO` could starve other
  system processes for no latency benefit, since discovery isn't on the
  hot path anyway.

**Why this matters for the benchmark (step 8):** if the initial
implementation runs everything on one thread (data + discovery +
callbacks combined), a discovery re-announce or a slow user callback can
silently blow the p99/worst-case latency numbers even though the "happy
path" average looks fine. Separating responsibilities per the above
before benchmarking makes tail-latency failures attributable to a
specific cause, rather than an unexplained occasional spike with no clear
source.

## Why (design rationale)

ROS1 is slow due to TCPROS per-message connection overhead and a central
master for topic registration. ROS2/DDS fixes some of this but introduces
continuous multicast discovery re-announcement (SPDP), which causes O(N²)
discovery traffic that scales badly and can flood constrained/WiFi networks.

ReLink avoids both problems by:
- Using UDP by default (no per-message handshake)
- Using a **one-shot, jittered multicast beacon** for discovery instead of
  ROS2's continuous re-announce — traffic does not scale with uptime, only
  with node count at boot/join time
- Matching topics directly in the beacon payload, so discovery and
  pub/sub interest-matching happen in a single step

## Type system: default primitive types + custom types (std_msgs naming)

ReLink topics need a declared type, similar to ROS `std_msgs`. v1 supports a
fixed set of default primitive and array types **named exactly like ROS
std_msgs** (https://github.com/ros/std_msgs/tree/noetic-devel/msg), plus
user-defined custom struct types built from those primitives.

Each default type follows std_msgs convention: a PascalCase message name
wrapping a single field called `data`, so anyone coming from ROS recognizes
the naming immediately.

### Default primitive types (mirrors ROS `std_msgs`)

```cpp
struct Bool    { bool     data; };
struct Byte    { uint8_t  data; };  // std_msgs/Byte
struct Char    { uint8_t  data; };  // std_msgs/Char
struct Int8    { int8_t   data; };
struct Int16   { int16_t  data; };
struct Int32   { int32_t  data; };
struct Int64   { int64_t  data; };
struct UInt8   { uint8_t  data; };
struct UInt16  { uint16_t data; };
struct UInt32  { uint32_t data; };
struct UInt64  { uint64_t data; };
struct Float32 { float    data; };
struct Float64 { double   data; };
```

### Default array types (mirrors ROS `std_msgs` MultiArray set)

```
ByteMultiArray
Int8MultiArray     Int16MultiArray     Int32MultiArray     Int64MultiArray
UInt8MultiArray    UInt16MultiArray    UInt32MultiArray    UInt64MultiArray
Float32MultiArray  Float64MultiArray
```

Each MultiArray type is a length-prefixed array of its primitive's raw
value type (`data` field), same as std_msgs's `<Type>MultiArray.data[]`:

```cpp
template<typename T>
struct MultiArray {
    uint32_t count;
    T data[]; // variable length, count elements
};
// e.g. MultiArray<uint8_t>  == UInt8MultiArray
// e.g. MultiArray<float>    == Float32MultiArray
```

This matches ROS `std_msgs` type names exactly (see
https://github.com/ros/std_msgs/tree/noetic-devel/msg), minus `String`,
`Header`, `Time`, `Duration`, and `ColorRGBA` — add those later only if a
real use case needs them; v1 stays numeric-only. Custom types (below) do
**not** follow the `data`-field convention — that's only for the default
primitive wrappers, matching std_msgs.

### Custom types

Custom types are **not built into the ReLink library** — they are defined
entirely in the user's own application code, as a plain struct composed of
the default primitive types above (and optionally default array types).
The library never needs to know the specific struct layout ahead of time;
it just treats any type meeting the rules below as serializable, via a
template — no editing library headers, no registering with a central type
list.

```cpp
// user's own header, e.g. my_robot_msgs.hpp — lives in the user's project,
// not inside ReLink itself
#pragma pack(push, 1)
struct ImuReading {
    float accel_x, accel_y, accel_z;
    float gyro_x, gyro_y, gyro_z;
    uint64_t timestamp_us;
};
#pragma pack(pop)

// user picks their own topic IDs too, e.g. in an enum in their own code
enum MyTopics : uint16_t {
    TOPIC_IMU = 100,
    TOPIC_TEMP = 101,
};
```

```cpp
// user's application code — no ReLink source changes needed
#include "relink/relink.hpp"
#include "my_robot_msgs.hpp"

RelinkNode node;
node.advertise<ImuReading>(TOPIC_IMU);
node.publish<ImuReading>(TOPIC_IMU, reading);
```

Rules for v1 (these are the *only* requirements ReLink's templates check —
anything meeting them just works, no boilerplate beyond the struct itself):
- Must be a **trivially copyable** plain struct (`std::is_trivially_copyable`)
  — this is what lets the library do a raw memcpy into the payload with no
  custom serialization code required from the user.
- No nested custom types (a custom type may not contain another custom
  type) — keep serialization a flat memcpy of the struct for speed.
- No dynamic-length fields inside a custom type except via the MultiArray
  wrapper above (which carries its own length prefix).
- Each topic declares exactly one type (default or custom) at registration
  time (`advertise<T>(topic_id)` / `subscribe<T>(topic_id, callback)`), so
  the receiver knows how to reinterpret the payload bytes without a runtime
  schema exchange.
- Struct packing/alignment must be fixed explicitly (e.g.
  `#pragma pack(push, 1)`) so the byte layout is identical across
  compilers/platforms — do not rely on default struct padding.

Because the type is a template parameter resolved at compile time in the
user's own translation unit, the ReLink library core (steps 1-6 below) must
be written so it never needs a `switch`/list of "known" custom types —
`advertise<T>`/`subscribe<T>`/`publish<T>` should compile against *any*
user struct meeting the rules above, via a `static_assert` on
`std::is_trivially_copyable<T>` rather than a fixed type registry.

## MTU limit and why UDP needs seq_num (but TCP won't)

**v1 hard constraint: one message must fit in one UDP datagram, under the
network MTU.** Typical Ethernet MTU is 1500 bytes; after IP+UDP headers
(28 bytes) and the ReLink frame overhead above, the safe payload budget is
roughly **1400-1450 bytes** — leave margin, don't assume the full 1500.
This is intentional, not an oversight: **v1 does not implement
fragmentation/reassembly across multiple UDP packets.** A message larger
than this limit must be rejected at `publish()` time with a clear error
(or split into multiple smaller topics/messages by the user), not silently
truncated or corrupted.

- Fragmenting large messages across multiple UDP datagrams (like IP
  fragmentation, or like ROS2/DDS handling large messages) is real,
  well-understood complexity — sequence numbering per fragment, reassembly
  buffers, handling a dropped fragment mid-message — and is explicitly
  **out of scope for v1** (add to the deferred list below). For typical
  small robotics messages (IMU, joint states, commands), this limit is
  rarely hit; large payloads (images, point clouds) are a deliberate
  future problem, not a v1 one.

**Why `seq_num` matters on UDP but becomes redundant on a future TCP
path:** UDP gives no delivery ordering or duplicate-detection guarantee —
two packets can arrive out of order, or (rarely) a duplicate can appear at
the network layer. `seq_num` lets the receiver notice this (e.g. "newer
seq_num arrived before an older one — discard the stale one" for
latest-value-wins topics) without needing a real reliability protocol.
**TCP already guarantees in-order, exactly-once delivery of the bytes on
its own stream** — that's TCP's own internal sequence numbering doing
the same job at the transport layer, before ReLink ever sees the bytes.
So if/when the deferred TCP reliable path is eventually built:
- `seq_num` in `RelinkHeader` becomes **redundant for ordering/dedup
  purposes** over that path — TCP has already guaranteed order and
  exactly-once delivery by the time ReLink reads it off the stream.
- Do **not** remove the field from the header, though — keeping the same
  `RelinkHeader` layout on both transports (rather than a UDP-only vs
  TCP-only header variant) keeps the wire format simpler and uniform,
  which matters more for the "one simple protocol, any language" goal
  than saving 2 bytes on the TCP path.
- `seq_num` can still be useful over TCP for the *user's own application
  logic* (e.g. detecting a gap if the sender itself skipped a value), just
  not for transport-level ordering/dedup — that distinction is worth a
  comment in the header definition so a future implementer doesn't
  misunderstand its purpose on the TCP path.

## Wire format (fixed header, all integers little-endian)

Every frame is bracketed by a fixed start byte and stop byte:

```
'#'  (0x23)  — start byte, always the first byte of a frame
'\n' (0x0A)  — stop byte, always the last byte of a frame
```

```
struct RelinkHeader {
    uint16_t topic_id;      // numeric ID, not a string — smaller, faster to parse
    uint16_t seq_num;       // for dedup / ordering awareness, not full reliability
    uint16_t payload_len;   // bytes following this header, before checksum/stop byte
    uint8_t  flags;         // bit 0: secure (AES-256-GCM), bit 1: checksum present
};
// full frame on the wire:
// '#' + RelinkHeader (7 bytes)
//     + [SecureExt, 8 bytes, if secure flag set — see Optional encryption
//        section below for its layout and why it's needed]
//     + payload (payload_len bytes)
//     + [auth tag, 16 bytes, if secure flag set]
//     + [checksum, 2 bytes, if checksum flag set]
//     + '\n'
// note: plaintext frames (secure flag unset, the default) are completely
// unaffected by SecureExt/auth tag — this only applies to secure=true.
```

**Optional checksum, user-selectable per topic (like `secure`):**

```cpp
// no checksum (default) — matches secure default of false, zero overhead
node.advertise<ImuReading>(TOPIC_IMU);

// checksum enabled
node.advertise<ImuReading>(TOPIC_IMU, /*secure=*/false, /*checksum=*/true);
```

- Algorithm: **CRC16** (e.g. CRC-16/CCITT) over `RelinkHeader + payload` —
  cheap to compute (a small lookup table, no crypto), catches accidental
  bit-flips/corruption on a plain UDP link. This is **not** a security
  mechanism — it does not protect against a deliberate attacker, only
  against ordinary transmission corruption. If tamper-resistance is
  needed, use `secure=true` (GCM's auth tag), not the checksum.
- **`secure=true` and `checksum=true` are independent flags** — GCM's
  16-byte auth tag already gives strong integrity, so enabling the 2-byte
  checksum on top of it is redundant (allowed, but adds nothing
  meaningful). The checksum's real use case is a topic that wants basic
  corruption detection **without** paying AES's CPU cost — e.g. a
  trusted LAN where tampering isn't a concern but flipped bits from noisy
  links still are.
- **Placement**: checksum is computed and appended **after** the payload
  (and after the auth tag, if present) — checked before the stop byte, at
  a fixed offset derived from `payload_len` and the `flags` byte, same
  "never scan, always compute the offset" rule as the stop byte itself.
- **Zero cost when disabled** (the default) — no CRC computation, no
  extra 2 bytes on the wire, consistent with the lightweight/1000Hz
  principle applied to every other optional feature in this spec.
- On checksum mismatch: drop the frame silently (same policy as a failed
  GCM auth check or a failed start/stop byte sanity check).

**Why include start/stop bytes when UDP datagrams are already discrete
(unlike TCP, which needs delimiters to find message boundaries in a
continuous byte stream):**
- **Cheap sanity check, not framing-for-discovery.** On receive, checking
  `buf[0] == '#'` and `buf[len-1] == '\n'` before touching `payload_len`
  is a near-zero-cost way to reject garbage/corrupted/non-ReLink UDP
  traffic on the same port before doing any real parsing work — a useful
  first line of defense, not the primary correctness mechanism (that's
  still `payload_len`, the optional checksum, and/or the GCM auth tag if
  `secure=true`).
- **Human-debuggable on the wire.** A `#`-prefixed, `\n`-terminated frame
  is easy to spot in a raw packet capture or a quick debug print, which
  plain binary framing without markers is not.
- **Forward-compatible if ReLink ever runs over a stream transport** (e.g.
  a future TCP path, or a serial/UART link for embedded use) — those
  *do* need explicit delimiters to find frame boundaries in a continuous
  stream, so having them defined now means the same framing rule works
  unchanged if the transport changes later.

**Sharp edge this introduces — must be handled correctly:** the payload
itself is raw binary and can legitimately contain the byte value `0x0A`
(`'\n'`) or `0x23` (`'#'`) as ordinary data, not just as framing markers.
Since `payload_len` and the `flags` byte are already known from the
header, **the receiver must never scan for `'\n'` to find the end of the
frame** — it must read: 8 bytes for `SecureExt` if `secure` is set, then
exactly `payload_len` bytes for the payload, then 16 more for the auth
tag if `secure` is set, then 2 more for the checksum if `checksum` is
set, and only *then* check that the next byte is `'\n'`. Scanning for the
stop byte instead of computing this offset would corrupt any payload that
happens to contain a `0x0A` byte internally. The start/stop bytes are a
boundary check performed at fixed, pre-computed offsets — never a
delimiter to search for.

Keep the header fixed and minimal. No versioning byte needed yet since v1 is
internal-only (no external clients) — revisit if external support is added
later.

## Discovery: build order — com-core first (mode A), then decentralized (mode B)

Two discovery modes exist (see earlier discussion), but **build them in
this order**: implement mode A (`relink-com-core`) first, since a central
signaling point is simpler to get working and debug — closer to how ROS's
master works, just leaner — then implement mode B (pure multicast, no
daemon) once the pub/sub core itself is proven correct against mode A.
This de-risks the harder, timing-sensitive decentralized logic by
validating everything else (topic matching, ring buffer, UDP send/recv)
against a simple, deterministic discovery path first.

### Mode A: relink-com-core (ACK-based central registration, like a lean ROS master)

`relink-com-core` is a small standalone daemon, one per deployment,
listening on a **fixed, well-known address:port** (configured, not
discovered). Every node talks to it via simple UDP request/ACK — no
multicast, no jitter, no timing complexity, since there's exactly one
node to talk to.

**Default port: `8445`.** This is the standard port `relink-com-core`
listens on unless overridden — pick one fixed number so it's documented
and predictable across deployments, the same way well-known services have
a default port. The IP address is always user-supplied (there's no way to
guess which machine on the network is running com-core), but the port
should not need specifying in the common case.

**User-facing config API — separate IP and port setters, port has a
default so it only needs setting on override:**

```cpp
RelinkNode node;

// common case: just set the IP, port stays at its default of 8445
node.set_com_core.ip("10.0.0.5");

// explicit port override, only needed if com-core runs on a non-default port
node.set_com_core.port(9000);
```

`set_com_core` is a small config sub-object exposed on `RelinkNode` (not
a free function) so IP and port can be set independently and in either
order, rather than forcing both into one call — this also leaves room
for future settings on the same sub-object (e.g. a registration timeout)
without growing the argument list of a single function.

```cpp
struct ComCoreConfig {
    void ip(const std::string& addr);   // required — no default, must be set
    void port(uint16_t p = 8445);       // optional — 8445 unless overridden
    // internally holds the resolved address:port used by discovery below
};
// exposed as a member on RelinkNode, e.g. `ComCoreConfig set_com_core;`
```

- Calling `node.set_com_core.port(...)` without ever calling
  `node.set_com_core.ip(...)` is a configuration error — the node has a
  port but no address to send to. Fail loudly at `spin()`/registration
  time with a clear message ("com-core IP not set"), not silently.
- If `set_com_core.ip(...)` is never called at all and
  `use_multicast_discovery()` (mode B) is also never called, the node
  must fail clearly at startup with "no discovery method configured" —
  never silently do nothing, since a node that can't discover peers but
  doesn't say so is a hard-to-debug failure mode.
- `relink-com-core` itself should also default to binding `0.0.0.0:8445`
  when started with no arguments, and accept an optional
  `--port <N>` flag to override — keeping the client default and the
  daemon default consistent (`8445` both sides) means the common case
  needs zero configuration beyond the IP.

```
struct RegisterRequest {
    uint32_t node_ip;
    uint16_t node_port;
    uint16_t topic_count;
    uint16_t topic_ids[topic_count]; // published or subscribed
};

struct RegisterAck {
    uint8_t  status;          // 0 = ok, 1 = error
    uint16_t peer_count;
    struct { uint32_t ip; uint16_t port; uint16_t topic_id; } peers[peer_count];
    // the current known peers relevant to this node's topic_ids
};
```

Behavior:
1. On startup, a node sends `RegisterRequest` to `relink-com-core`'s
   fixed address, listing every topic it publishes/subscribes to.
2. `com-core` replies with `RegisterAck` — the ack itself carries the
   current roster of matching peers for those topics, so registration and
   discovery happen in one request/response round trip (no separate
   "ask for peers" step).
3. `com-core` also updates its internal topic→peer table with this node's
   info, so *future* registrants immediately learn about this node too.
4. **Retry with backoff if no ACK arrives** (e.g. 3 retries, exponential
   backoff, then give up and log an error — a node should not hang
   forever waiting on `com-core`) — this is the one piece of retry logic
   this mode needs, since everything else is a straightforward
   request/reply, not a distributed protocol.
5. Once a node has its peer list from the ACK, it proceeds exactly like
   mode B from here on — all further data traffic is direct unicast UDP,
   `com-core` is never touched again per-message, only at
   registration/re-registration.
6. **`com-core` does not sit in the data path.** Its only job is
   bootstrapping the peer table once at startup — this preserves the
   "no per-message central dependency" principle even though a central
   component exists for discovery.

This mode is simpler to implement and debug first: no jitter timing, no
"did my one-shot beacon get lost" uncertainty, no boot-storm collision
handling — just a request, a reply, and retry-on-timeout. Get the rest of
the pipeline (topic matching, ring buffer, UDP data path) working and
tested against this mode before moving to mode B.

### com-core must run standalone on any device — build it in C++ and Python

`relink-com-core` is a small, isolated daemon — it never touches the
templated type system, the ring buffer, or `advertise`/`subscribe`, only
`RegisterRequest`/`RegisterAck` UDP packets and a topic→peer table. That
narrow scope is exactly what makes it practical to implement twice, in
two languages, from day one rather than treating a second language as a
"later" concern like the node-side bindings:

- **C++ build**: for embedded boards or a robot's main compute unit where
  you want it compiled into the same toolchain as the rest of the robot
  stack, with no runtime dependency beyond what's already used elsewhere.
- **Python build**: for running com-core on a device that doesn't have
  (or doesn't want) a C++ build step at all — a lab laptop, a Raspberry Pi
  used purely as a coordination point, a CI test environment, or any
  quick ad-hoc deployment where "just run a script" beats "cross-compile
  a binary." Python's standard `socket` + `struct` modules cover
  everything `com-core` needs — no extra dependency, consistent with the
  no-framework rule applied to every other language binding in this spec.
- **Both implementations must produce byte-identical
  `RegisterRequest`/`RegisterAck` packets** — this is just the general
  "protocol, not library" rule from earlier in this spec applied to
  com-core specifically: a C++ node must be able to register with a
  Python-run com-core and vice versa, with neither side aware which
  language the other side is written in.
- **Keep the C++ and Python implementations independent**, same as the
  rule for node bindings — the Python version is not a wrapper calling
  into the C++ binary, and the C++ version doesn't shell out to Python.
  Each is a full, small, standalone implementation of the same
  `RegisterRequest`/`RegisterAck` spec.
- Because com-core's logic is genuinely small (parse a request, update an
  in-memory table, send a reply), a Python implementation should be well
  under 200 lines — this isn't extra scope, it's confirmation that the
  protocol stayed simple enough to hand-implement anywhere, the same test
  applied to every other language-portability decision in this spec.

### Mode B: one-shot jittered multicast beacon (no daemon, build second)

Once mode A proves the pub/sub core works end to end, implement this
fully decentralized alternative — same topic-matching outcome, no
`com-core` dependency, but more timing complexity (jitter, retries,
late-joiner safety net) since there's no single point coordinating state.

Beacon payload (sent over UDP multicast, e.g. 239.255.0.1:7400 — pick and
document an actual address/port):

```
struct BeaconPacket {
    uint32_t node_ip;        // sender's own IP
    uint16_t node_port;      // sender's own UDP port for data traffic
    uint16_t topic_count;
    uint16_t topic_ids[topic_count]; // topics this node publishes OR subscribes to
};
```

Behavior:
- On startup, node sends this beacon **3 times** with random jitter
  (0–200ms between attempts) to avoid boot-storm collisions when multiple
  nodes start simultaneously.
- Every listening node compares `topic_ids` against its own local
  publish/subscribe list.
  - **No overlap** → discard the beacon immediately, keep no state.
  - **Overlap found** → store `(topic_id -> node_ip:node_port)` in a local
    peer table. All further traffic for that topic goes via direct unicast
    UDP to that address — never via multicast again.
- **Late-subscriber safety net**: every node also re-sends its beacon every
  30–60 seconds (sparse, not a heartbeat loop) so a node that joins after
  the initial burst still gets discovered. This interval must stay low-cost
  — do not shorten it without reason; this is what keeps ReLink from
  repeating ROS2's mistake.
- Subscribers beacon their own topic interest too (not just publishers) —
  discovery must work in both directions.

### Choosing between modes at runtime — must be unambiguous, never inferred

Both modes must produce the exact same result in the node's local peer
table (`topic_id -> ip:port`) — everything downstream (UDP data path,
ring buffer, callbacks) is identical regardless of which mode populated
that table.

**The two modes are mutually exclusive per node, and the API must make
this impossible to get wrong by accident:**

```cpp
RelinkNode node;

// mode A — configure com-core
node.set_com_core.ip("10.0.0.5");   // port defaults to 8445

// mode B — decentralized multicast, no daemon
node.use_multicast_discovery();
```

- **Calling both on the same node is a configuration error, not a
  "last one wins" or "try both" situation.** If `set_com_core.ip(...)`
  has been called and the user then also calls
  `use_multicast_discovery()` (or vice versa), fail immediately and
  loudly — e.g. throw/return an error at the second call, with a message
  like `"discovery mode already set to com-core; cannot also enable
  multicast discovery on the same node"`. Silently picking one (say,
  "com-core wins if both are set") would let a config mistake go
  unnoticed until a discovery failure shows up much later, in a much
  more confusing form.
- **Calling neither is also a configuration error**, not a silent
  fallback to one or the other — already specified above
  ("no discovery method configured" at startup/registration time). A
  node should never guess which mode the user meant.
- **Internally, track the selected mode as an explicit enum**
  (`DiscoveryMode::None`, `::ComCore`, `::Multicast`), not as "whichever
  config fields happen to be non-empty." Checking "is `com_core_ip`
  non-empty" as a proxy for "mode is ComCore" is fragile — an explicit
  enum, set exactly once by whichever setter is called first, makes the
  mutual-exclusivity check above trivial to implement correctly (check
  the enum is still `None` before allowing a mode-selecting call to
  proceed) and impossible to accidentally infer wrong.
- **This check must run at config time (the setter call itself), not
  deferred to `spin()`/registration time** — the user should get the
  error the moment they make the conflicting call, at the exact line
  that caused it, not several lines later when the node actually starts
  running. Fast, precise feedback here directly prevents the "node runs
  for a while doing something the user didn't intend" confusion this
  section is meant to eliminate.

## Buffer: fixed-size ring buffer per topic

- Pre-allocate a fixed-capacity ring buffer per topic (e.g. 8 slots to
  start — make it configurable but do not implement auto-growth in v1).
- On overflow, **drop the oldest** message, never block the publisher.
- No dynamic resizing logic yet.

## Task list (implement in this order)

1. Define `RelinkHeader` and `BeaconPacket` structs exactly as above, in a
   shared header file. Also define the default primitive types, the
   `MultiArray<T>` template, and packing rules (`#pragma pack`) from the
   type system section above — this is the foundation everything else
   serializes against.
2. Implement the fixed-size ring buffer, templated or per-topic, with
   drop-oldest-on-overflow behavior. Unit test overflow behavior explicitly.
3. Implement the raw UDP send/receive core (v1 note: build and benchmark
   this with `secure=false` only — `SecureExt` and the nonce logic are
   deferred, see Optional encryption below; this step's MTU/offset math
   should assume no `SecureExt`/auth tag present for now). **Structure
   this as the dedicated data thread from the Threading and core
   allocation section above from the start** — do not build it as
   single-threaded code you plan to split out later, since retrofitting
   thread separation after the fact is more error-prone than designing it
   in from step 3:
   - Bind a UDP socket for data traffic.
   - In `publish()`, **check the total frame size against the MTU budget
     (~1400-1450 bytes payload, minus 8 bytes if secure and 2 if checksum
     — see note below) before sending — reject with a clear error if the
     message is too large**, rather than sending and letting the OS
     silently fragment at the IP layer (IP fragmentation is fragile and
     reassembly failures are hard to debug — better to fail loudly at the
     API boundary).
   - Pack `'#'` + `RelinkHeader` (including the `flags` byte) +
     `[SecureExt if secure]` + payload bytes + `[auth tag if secure]` +
     `[CRC16 if checksum]` + `'\n'` and send to a resolved peer address.
   - On receive: check `buf[0] == '#'`, read `payload_len` and `flags`
     from the header, compute the stop-byte offset as
     `1 + sizeof(RelinkHeader) + (8 if secure) + payload_len + (16 if
     secure) + (2 if checksum)`, and check the byte there equals `'\n'`
     — never scan for `'\n'`, since payload bytes can legitimately
     contain `0x0A`. Verify the checksum (if present) and the GCM auth
     tag (if secure) before handing off. Drop the frame silently on any
     check failure. Otherwise parse the header, look up the topic's ring
     buffer, push the payload.
4. Implement `relink-com-core` in **both C++ and Python** (small, isolated
   scope — see the section above on why this is practical to do twice
   from the start, unlike the node-side bindings which stay C++-only for
   now):
   - Listens on `0.0.0.0:8445` by default (overridable via `--port`) for
     `RegisterRequest`; client-side `set_com_core.port(...)` defaults to
     the same `8445` so no port config is needed in the common case.
   - Maintains an in-memory topic→peer table.
   - On each request, updates the table with the sender's info, then
     replies with `RegisterAck` containing the current matching peers.
   - Implement client-side retry with exponential backoff (e.g. 3 tries)
     for the case where no ACK arrives.
   - Confirm both implementations produce byte-identical wire packets —
     test a C++ node registering against the Python com-core, and a
     C++ com-core answering a request built by a small Python test
     script, before trusting either implementation.
   - Get a two-node test working through com-core before moving on — this
     validates topic matching and peer table logic in the simplest
     possible setting (one central point, no timing races).
5. Implement mode B (multicast beacon), now that the pub/sub core is
   proven correct against mode A:
   - Beacon sender: joins/sends to the chosen multicast group, 3x with
     jitter on startup, repeats every 30–60s afterward (sparse
     re-announce).
   - Beacon listener: parses incoming `BeaconPacket`s, matches
     `topic_ids` against the local publish/subscribe list, stores/updates
     `topic_id -> ip:port` in the local peer table on match, discards
     with no state kept on no match.
   - Confirm this produces the same peer-table outcome as mode A did, on
     the same two-node test.
6. Wire it together into a minimal public API, templated on the declared
   topic type so the caller gets compile-time type safety, and so **any
   user-defined struct works without touching library code** — enforce
   this with `static_assert(std::is_trivially_copyable<T>::value)` inside
   `advertise`/`subscribe`/`publish` rather than a fixed type list. Works
   the same whether the type is a default std_msgs-style type or a custom
   struct the user defined in their own project:
   ```cpp
   RelinkNode node;

   // default type, std_msgs-style naming
   node.advertise<Float32>(topic_id_temp);
   node.publish<Float32>(topic_id_temp, Float32{ .data = 36.6f });

   // custom type
   node.advertise<ImuReading>(topic_id_imu);
   node.subscribe<ImuReading>(topic_id_imu, callback_fn);
   node.publish<ImuReading>(topic_id_imu, reading);

   node.spin(); // runs beacon + recv loop
   ```
   Internally this should just be a fixed-size memcpy of the templated
   type into the payload — no runtime type dispatch needed since the type
   is fixed per topic at registration.
7. Write a small two-process test: one publisher process, one subscriber
   process, same machine or LAN, confirm discovery + delivery works end to
   end without any manually-configured peer address. **Also test the
   mutual-exclusivity error paths explicitly**: calling
   `set_com_core.ip(...)` then `use_multicast_discovery()` (and the
   reverse order) must fail immediately at the second call with a clear
   error, and calling neither must fail clearly at
   `spin()`/registration time — write these as actual unit tests, not
   just something checked by eye once.
8. **Benchmark against the 1000 Hz / ≤1ms performance target above.** Run
   the publisher at a sustained 1000 Hz publish rate with a realistic
   payload (e.g. `ImuReading`-sized, tens of bytes) over actual LAN (not
   localhost loopback only — loopback hides real NIC/OS scheduling
   overhead) and measure:
   - round-trip / one-way latency distribution (p50, p99, worst-case, not
     just average — a single missed deadline at 1000Hz matters)
   - whether the rate is actually sustained over a real duration (e.g. 60s
     continuous), not just a short burst
   - CPU usage on both ends
   If this target isn't met, profile before optimizing blindly — check
   for the specific culprits called out in the Performance target section
   (heap allocation in the hot path, blocking recv, serialization cost)
   rather than assuming the whole architecture needs a rewrite.
9. Once the 1000Hz target is confirmed, benchmark against an equivalent
   ROS1 or ROS2 topic at the same message size/rate on the actual target
   hardware, to validate the "faster than ROS" comparison specifically —
   do this before adding anything beyond v1 scope.
10. If the 1000Hz target is met but tail latency (p99/worst-case) is
   inconsistent, or you want more headroom, apply the Lean optimization
   techniques above **one at a time**, re-benchmarking after each — start
   with CPU pinning and the lock-free ring buffer (highest leverage for
   jitter), then `recvmmsg` batching and socket buffer sizing if syscall
   overhead shows up in profiling.
11. Stress-test discovery under real conditions: power-cycle a node mid-run,
   drop WiFi for a few seconds, boot 5+ nodes simultaneously. Confirm the
   jitter/re-announce behavior holds up before considering it done.

## Optional encryption (AES-256-GCM, opt-in per topic, deferred past v1 core)

Not part of the initial 1000Hz/lightweight core build (see Explicitly
deferred below), but specified now so the API shape is decided upfront and
doesn't require breaking changes later.

**User-facing API: a single boolean, nothing more.**

```cpp
RelinkNode node;

// plaintext, default — zero overhead, same as everything above
node.advertise<ImuReading>(TOPIC_IMU);

// encrypted — same call, one extra argument
node.advertise<ImuReading>(TOPIC_IMU, /*secure=*/true);
node.subscribe<ImuReading>(TOPIC_IMU, callback_fn, /*secure=*/true);
```

No key exchange handshake, no config file format, no certificate concept —
the key itself is supplied once, out of band, when `secure=true` is used
for the first time in the process (e.g. `node.set_key(my_256_bit_key)` at
startup, or an environment variable read once at init). This keeps the
"no per-message connection setup" principle intact — the key is a
pre-shared static value the user manages, not something ReLink negotiates.

**BUG FIX — nonce reuse, corrected here (this only touches the `secure=true`
path; plaintext topics and the 1000Hz hot path are completely unaffected,
so the performance target is not at risk):**

The original design derived the 96-bit GCM nonce from `topic_id + seq_num`.
`seq_num` is only 16 bits — at a sustained 1000Hz publish rate it wraps
around in under 66 seconds, which means the same nonce gets reused under
the same key. **Nonce reuse under GCM is a critical security break**, not
a minor issue — it can leak the authentication key and allow forged
messages. This must be fixed before `secure=true` is implemented.

**Fix: a wider, dedicated nonce counter that exists only on secure frames.**

```
struct RelinkHeader {
    uint16_t topic_id;
    uint16_t seq_num;
    uint16_t payload_len;
    uint8_t  flags;         // bit 0: secure, bit 1: checksum
};
// when flags & SECURE_BIT is set, immediately after the header:
struct SecureExt {
    uint32_t session_id;    // random, generated once at process startup
    uint32_t nonce_counter; // monotonic per-topic counter, increments every
                             // secure publish on this topic, independent of seq_num
};
// full frame when secure: '#' + RelinkHeader + SecureExt + payload
//     + auth tag (16B) + [checksum if set] + '\n'
```

- **`session_id`** (32-bit, random, generated once when the node starts)
  guarantees a fresh nonce space even if the process restarts and reuses
  the same static key — this is the piece the original design was missing
  entirely, not just the counter width.
- **`nonce_counter`** (32-bit, starts at 0, increments once per secure
  publish on that topic) replaces `seq_num` as the nonce's counter
  component — at 1000Hz sustained, this wraps after roughly **49 days**
  of continuous publishing on one topic, versus the old design's 66
  *seconds*. If a topic could realistically run continuously for 49+
  days, add a check that rotates `session_id` (re-randomize) before the
  counter wraps, and document that as a follow-up — not needed for
  typical robotics session lengths, but worth a code comment so a future
  implementer doesn't reintroduce the bug at a longer time horizon.
- **Nonce = `session_id (32 bits) || nonce_counter (32 bits) || topic_id
  (16 bits, zero-extended to fill 96 bits)`** — still fully derived from
  fields already on the wire, so the receiver reconstructs the exact same
  nonce the sender used, with no extra round trip.
- **Cost is confined entirely to the `secure=true` path**: `SecureExt` is
  8 extra bytes, only present when the secure flag is set — on top of the
  16-byte auth tag you already had budgeted. A plaintext topic's frame,
  parsing, and hot-path cost are **byte-for-byte identical to before this
  fix** — nothing here touches `RelinkHeader`'s size, the ring buffer, or
  the 1000Hz benchmark path.

**What happens internally when `secure=true` (must stay invisible to the
user beyond the one flag):**
- Algorithm: **AES-256-GCM** (authenticated encryption — confidentiality
  and integrity in one pass, not plain AES-CBC which has no integrity
  check).
- Only the **payload** is encrypted; `RelinkHeader` and `SecureExt` stay
  in plaintext, since the receiver needs `topic_id`/`payload_len` to
  route and size the message, and needs `session_id`/`nonce_counter` to
  reconstruct the nonce, before it can even attempt decryption.
- On the receive side, GCM's authentication check must be verified before
  the payload is handed to the user's callback — a failed auth check means
  drop the packet silently (log if debug), never pass unauthenticated
  bytes through as if they were valid.
- This must **not** run in the same hot path as plaintext topics — a
  `secure=false` topic must pay zero AES cost and zero `SecureExt`
  parsing cost, so the encrypt/decrypt step should be a branch taken only
  per-topic at setup, not a per-message runtime check that adds branching
  overhead to the plaintext fast path.

**Explicitly out of scope for this feature, even when built:**
- No key exchange/negotiation protocol — pre-shared key only.
- No per-message key rotation — a static key for the process lifetime is
  the v1 assumption; rotate by restarting nodes with a new key if needed.
- No asymmetric/public-key crypto — symmetric AES only, matching the
  "lightweight, no heavy framework" principle applied elsewhere in this
  spec.

## Explicitly deferred (do not build yet)

- TCP reliable transport path — if a message must arrive, prefer a cheap
  app-level fix first (resend same UDP packet 2–3x with seq_num dedup)
  before building a full TCP path.
- Multi-language codegen for the type schema — hand-write C++ and Python
  bindings against the fixed struct layout for now.
- External raw-socket / third-party client support and wire format
  versioning — only needed once something outside your own codebase must
  talk to ReLink directly.
- AES-256-GCM encryption (`secure=true`) — API shape and the corrected
  nonce design (`session_id` + `nonce_counter` via `SecureExt`, fixing the
  earlier seq_num-reuse bug) are decided (see Optional encryption section
  above) so it can be added without breaking changes, but implementation
  comes after the plaintext 1000Hz core is validated, not alongside it.
- Fragmentation/reassembly for messages larger than one UDP datagram's
  MTU budget (~1400-1450 bytes payload) — v1 rejects oversized messages
  at `publish()` time instead; revisit only if a real use case (image,
  point cloud) needs larger single messages.

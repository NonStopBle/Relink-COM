<div align="center">

# ReLink

**A lightweight, ROS-like publish/subscribe protocol over UDP — built to be
faster and simpler than ROS1/ROS2.**

![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)
![Python](https://img.shields.io/badge/Python-3.8%2B-blue)
![Transport](https://img.shields.io/badge/transport-UDP-orange)
![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20Windows-lightgrey)

You do not need ROS installed, and you do not need any other extra
software to make it work. It comes in two versions — **C++** and
**Python** — that can talk to each other directly, so you can pick
whichever language fits your project.

</div>

---

## Table of contents

- [Words you might not know yet](#words-you-might-not-know-yet)
- [Step 0 — What ReLink actually is](#step-0--what-relink-actually-is)
- [Step 1 — Get the code](#step-1--get-the-code)
- [Step 2 — Zero to one (run it)](#step-2--zero-to-one-run-it)
- [Step 3 — Pick a discovery mode](#step-3--pick-a-discovery-mode)
  - [Pairing topics onto one port under `set_multiplex(false)`](#pairing-topics-onto-one-port-under-set_multiplexfalse)
  - [`network_id` — domain isolation for Mode B (multicast)](#network_id--domain-isolation-for-mode-b-multicast)
- [Step 4 — Named topics](#step-4--named-topics)
- [Step 5 — Quick start: C++](#step-5--quick-start-c)
- [Step 6 — Quick start: Python](#step-6--quick-start-python)
- [Step 7 — Message types](#step-7--message-types)
- [Step 8 — Sending images](#step-8--sending-images)
- [Step 9 — Running the rlcore daemon](#step-9--running-the-rlcore-daemon)
  - [Using `rl_topic` — listing and inspecting topics](#using-rl_topic--listing-and-inspecting-topics)
- [Step 10 — All examples](#step-10--all-examples)
- [Step 11 — Testing](#step-11--testing)
- [Step 12 — Benchmarks](#step-12--benchmarks)
- [Step 13 — NAT traversal (cross-network nodes)](#step-13--nat-traversal-cross-network-nodes)
- [Step 14 — Troubleshooting](#step-14--troubleshooting)
- [Step 15 — Technical deep dive](#step-15--technical-deep-dive)
- [Reference](#reference)
  - [Feature matrix](#feature-matrix)
  - [Repository layout](#repository-layout)
  - [Status / scope](#status--scope)

---

## Words you might not know yet

This doc assumes you already know the basics of networking (IP, port,
UDP/TCP) and software engineering, plus some robotics/mechatronics
background (sensors, control loops, maybe ROS). A few terms below are
specific to ReLink or used loosely elsewhere — worth a quick skim.

| Word | What it means here |
|---|---|
| **Daemon** | A small program that just runs in the background and waits to be asked for something — not something you interact with directly while it runs. |
| **Discovery** | The process of a node finding out another node's IP/port before either can send it anything. ReLink does this itself; you don't hand-configure addresses. |
| **Rendezvous point** | A single known, reachable address that other nodes register with so they can find each other — that's `rlcore`'s whole job (Step 9), nothing more. |
| **Multicast** | One node sending a message that every node on the local network can receive at once, without addressing each one individually. |
| **Publish/subscribe (pub/sub)** | A messaging pattern: a node "publishes" data on a named topic, and any number of other nodes "subscribe" to that topic — publishers and subscribers don't need to know about each other directly. |
| **Topic** | The named (or numbered) channel a message is published/subscribed on — same concept as a ROS topic. |
| **Wire format** | The exact byte layout of a message as it's actually sent over the network — not just "whatever the struct looks like in memory," but a documented, fixed layout both sides agree on. |
| **NAT traversal / hole punching** | Techniques for getting two nodes on different networks (e.g. behind different home routers) to reach each other directly, despite neither having a public IP — covered in Step 13. |

Come back to this table any time a word below doesn't make sense yet.

---

## Step 0 — What ReLink actually is

**Description:** The core vocabulary and the why-this-exists case, before any code.
**Tutorial Level:** Beginner

**Next ▶:** [Step 1 — Get the code](#step-1--get-the-code)

| Word | What it actually means |
|---|---|
| **Node** | Any process using `RelinkNode` (C++) or `RelinkNode` (Python). No node type distinction — every node can advertise, subscribe, and publish at once. |
| **Topic** | A named or numbered channel. One message type per topic, fixed at registration — not renegotiable later. |
| **Discovery** | How nodes find each other's address before they can exchange messages. ReLink has exactly two modes (Step 3), and you always pick one yourself — it's never guessed automatically. |
| **Wire format** | The literal byte layout of every packet — documented, not just implied by a struct definition. A from-scratch reimplementation in any language that can open a UDP socket can speak ReLink. |
| **rlcore** | The optional small daemon used for Mode A discovery. Not required — Mode B (multicast) needs no daemon at all. |

> **Why this exists, in short:** ROS1 and ROS2 are built to be general —
> any transport, any QoS policy, any serialization — and that generality
> costs speed and simplicity. ReLink gives up most of that generality on
> purpose (one fixed message type per topic, no built-in retry) to get a
> much simpler, much faster path instead. Measured head-to-head against
> ROS2 Humble on identical hardware/payload/rate (Step 12): **~3x lower
> average latency, ~10x lower worst-case tail latency**. The full
> technical reasoning is in [Step 15 — Technical deep
> dive](#step-15--technical-deep-dive).

If your system needs TCP reliability, arbitrary QoS policies, or schema
evolution, ROS2/DDS is the more complete answer. If your system needs a
`1000Hz+`, sub-millisecond, small-message control loop between a known set
of nodes on a LAN — sensor fusion, joint-state streaming, a control loop
between a planner and a driver — this is built specifically for that case.

### Review

- **Node** — a process, running `RelinkNode`, that can advertise, subscribe, and publish at once.
- **Topic** — a named or numbered channel, one message type, fixed at registration.
- **Discovery** — exactly two modes, chosen explicitly, never auto-negotiated (Step 3).
- **rlcore** — the optional daemon for Mode A; Mode B (multicast) needs none.

Now that you understand what ReLink actually is, let's get the code and run it.

---

## Step 1 — Get the code

**Description:** Cloning the repo — there is no package manager install.
**Tutorial Level:** Beginner

**◀ Previous:** [Step 0 — What ReLink actually is](#step-0--what-relink-actually-is) &nbsp;|&nbsp; **Next ▶:** [Step 2 — Zero to one (run it)](#step-2--zero-to-one-run-it)

```bash
git clone https://github.com/NonStopBle/Relink-COM.git
cd Relink-COM
```

This is currently the only way to install it — there is no package
manager entry, on purpose (see Step 0: no codegen, no dependency surface
beyond a socket).

### Building the C++ library, per OS

The C++ side is header-only (`cpp/include/`) — there's no
library to link, just headers to compile against. Python needs no
build step at all on any OS (pure stdlib). `camera_stream.cpp` is the
one exception below: it needs OpenCV, installed separately, on every
OS.

**Linux** — a C++17 compiler is usually already installed:

```bash
sudo apt-get install -y g++          # if you don't already have one
g++ -std=c++17 -I cpp/include -pthread cpp/examples/hello_relink.cpp -o hello_relink
```

**macOS** — Apple Clang (from Xcode Command Line Tools) works as-is;
no `-pthread` flag needed (macOS links pthreads by default):

```bash
xcode-select --install                # if you don't already have the toolchain
clang++ -std=c++17 -I cpp/include cpp/examples/hello_relink.cpp -o hello_relink
```

macOS shares the same POSIX sockets code path as Linux. The one
difference: `pthread_setaffinity_np()` (CPU core pinning, Step 12's
`-pthread` benchmark path) is a Linux-only glibc extension with no
macOS equivalent, so pinning is silently skipped there — everything
else (discovery, pub/sub, NAT traversal, `rlcore`/`relink-relay`)
works the same. This path is believed correct (it's the same POSIX
sockets/pthreads code Linux uses) but hasn't been run on real macOS
hardware — if you hit something, please open an issue.

**Windows** — two supported ways to get a C++17 compiler:

```bash
# Option A: MSYS2/MinGW-w64 (recommended -- closest to the Linux/macOS
# build commands above, no Visual Studio needed)
pacman -S mingw-w64-x86_64-gcc
g++ -std=c++17 -I cpp/include cpp/examples/hello_relink.cpp -o hello_relink.exe -lws2_32

# Option B: cross-compile FROM Linux/WSL for Windows (what this
# project's own CI/verification used -- see Step 11)
sudo apt-get install -y g++-mingw-w64-x86-64-posix
x86_64-w64-mingw32-g++ -std=c++17 -I cpp/include hello_relink.cpp -o hello_relink.exe -lws2_32
```

`-lws2_32` (Winsock) is required on Windows — there's no equivalent
flag on Linux/macOS since sockets are already part of libc there.
MSVC (`cl.exe`) is not tested but should work with the same `-lws2_32`
equivalent (`ws2_32.lib`) and no other changes, since the Windows code
path (`relink/platform.hpp`) is plain Win32/Winsock API, not
MinGW-specific.

**Verified, not just theorized**: the Windows build was cross-compiled
with MinGW-w64 and actually run under Wine — every unit/integration
test passing, plus live cross-platform pub/sub (a Windows `.exe` and a
native Linux binary exchanging messages over multicast in both
directions) and a 6-node concurrent stress test (3 Windows + 3 Linux
processes, 30s, zero crashes/errors). See [Step 15's Technical deep
dive](#step-15--technical-deep-dive) for anything platform-specific
that came up (a Windows header name collision, Winsock's different
`SO_RCVTIMEO` type, no true `sendmsg()` scatter-gather).

### Using CMake instead of raw compiler commands

The commands above call the compiler directly, which is fine for one
file. For a real project, [`cpp/cmake_example/`](cpp/cmake_example/) is
a copy-pasteable CMake project that builds the same `hello_relink.cpp`
on all three OSes from one `CMakeLists.txt` (it handles `-lws2_32` on
Windows and `pthread` on Linux/macOS for you):

```bash
cd cpp/cmake_example
cmake -B build .
cmake --build build
./build/hello_relink        # build/hello_relink.exe on Windows
```

Cross-compiling for Windows from Linux/WSL works the same way, via the
included toolchain file (this is exactly how the Windows build in this
README was produced and verified under Wine — see below):

```bash
sudo apt-get install -y g++-mingw-w64-x86-64-posix   # once
cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=mingw-w64-toolchain.cmake .
cmake --build build-win
wine build-win/hello_relink.exe   # or copy the .exe to a real Windows machine
```

To use this as a template for your own project: copy the
`cpp/cmake_example/` directory, change `RELINK_INCLUDE_DIR` to wherever
you've vendored `cpp/include/`, and change the one
`add_executable(...)` line to point at your own `.cpp` file(s) instead
of `hello_relink.cpp`.

### Building everything in this repo with CMake

If you're working inside this repo rather than starting a new project,
[`cpp/CMakeLists.txt`](cpp/CMakeLists.txt) builds the whole `cpp/`
tree at once — the header-only core, `relink-rlcore`/`relink-relay`
(via `cpp/rlcore/`'s own `CMakeLists.txt`), `rl_topic`, and every
example and test — instead of compiling files one at a time:

```bash
cd cpp
cmake -B build .
cmake --build build
ctest --test-dir build          # runs the 10 self-contained unit tests
```

`-DRELINK_BUILD_EXAMPLES=OFF`/`-DRELINK_BUILD_TESTS=OFF` skip those
directories if you only want the daemon/relay/rl_topic. `camera_stream`
(Step 8) is only built when `find_package(OpenCV)` succeeds — everything
else builds with no extra dependencies either way. The same MinGW-w64
toolchain file from `cpp/cmake_example/` cross-compiles this whole
project for Windows too:

```bash
cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake_example/mingw-w64-toolchain.cmake .
cmake --build build-win
```

[`cpp/build.sh`](cpp/build.sh) wraps all of the above into one script,
if you'd rather not type the cmake invocations yourself:

```bash
./cpp/build.sh                 # native build
./cpp/build.sh --windows       # cross-compile for Windows (needs g++-mingw-w64-x86-64-posix)
./cpp/build.sh --test          # native build, then run ctest
./cpp/build.sh --clean         # wipe build/ and build-win/ first
./cpp/build.sh --help          # full option list
```

### Copying the C++ library into your own project

ReLink's C++ core is header-only — there's no library to build or link,
just one directory to copy. This is all you need to use it outside
this repo:

1. **Copy `cpp/include/relink/`** into your own project (nothing else
   in this repo — `rlcore/`, `examples/`, `tests/` — is required just
   to use ReLink as a library):

   ```bash
   cp -r cpp/include/relink /path/to/your_project/third_party/relink
   ```

2. **Point your build's include path at the PARENT directory** you
   copied it into (`third_party`, not `third_party/relink`), since
   every header includes its siblings as `#include "relink/xyz.hpp"`:

   ```bash
   g++ -std=c++17 -I third_party -pthread my_node.cpp -o my_node
   ```

   ```cmake
   target_include_directories(my_target PRIVATE third_party)
   ```

3. **`#include "relink/relink.hpp"`** and use `RelinkNode` — see
   [`cpp/src/pub.cpp`](cpp/src/pub.cpp)/[`cpp/src/sub.cpp`](cpp/src/sub.cpp)
   for the smallest working example.

4. **Windows** additionally needs `-lws2_32` (or
   `target_link_libraries(... ws2_32)` in CMake) — the same headers
   work unmodified on Linux, macOS, and Windows otherwise.

Verified: copying just `cpp/include/relink/` to a throwaway directory
outside this repo and compiling/running a small node against it with
the exact `g++` command above works with no other changes. If you'd
rather start from a working project than wire this up by hand, copy
[`cpp/cmake_example/`](cpp/cmake_example/) instead — it's already set
up this way, just pointed at `../include` inside this repo instead of
a copied `third_party/relink/`. See [`cpp/README.md`](cpp/README.md#copying-relink-into-your-own-project)
for the same walkthrough alongside the rest of the C++ directory's
documentation.

Now that you have the code, let's run it.

---

## Step 2 — Zero to one (run it)

**Description:** Run the same tiny program twice and watch two nodes find each other with zero config.
**Tutorial Level:** Beginner

**◀ Previous:** [Step 1 — Get the code](#step-1--get-the-code) &nbsp;|&nbsp; **Next ▶:** [Step 3 — Pick a discovery mode](#step-3--pick-a-discovery-mode)

The fastest way to see it work: run the same tiny program twice (two
terminals, or two computers on the same network) and watch them find each
other and start talking — no daemon, no config file, no manual IP address.

| | C++ | Python |
|---|---|---|
| **Build** | `g++ -std=c++17 -I cpp/include -pthread cpp/examples/hello_relink.cpp -o hello_relink` | nothing to build — pure stdlib |
| **Run (in two terminals)** | `./hello_relink` | `python3 python/relink_py/examples/hello_relink.py` |

Within a second or two, each window prints `sent: N (to 1 peer(s))` and
`received: N` — the two copies found each other over multicast and are
exchanging a counter, once a second, in both directions.

**Check it worked**: if step 2 never prints `received:` on either side,
see [Step 14 — Troubleshooting](#step-14--troubleshooting).

**Why it worked**: `hello_relink.{cpp,py}` is under 60 lines. Open it and
read it — the whole API surface is `advertise`, `subscribe`, `publish`,
`spin`. Change `TOPIC_HELLO`, the message type, or what happens on
receive, and you're writing your own ReLink node.

### Review

- `advertise`/`subscribe`/`publish`/`spin` is the entire API surface needed to get two nodes talking.
- No daemon, no config file, no manual IP address — Mode B (multicast) discovery finds peers on its own.

Now that you've seen it work, let's look at how discovery mode selection actually works.

---

## Step 3 — Pick a discovery mode

**Description:** Choosing rlcore (Mode A) vs. multicast (Mode B), plus topic pairing and network_id domain isolation.
**Tutorial Level:** Beginner

**◀ Previous:** [Step 2 — Zero to one (run it)](#step-2--zero-to-one-run-it) &nbsp;|&nbsp; **Next ▶:** [Step 4 — Named topics](#step-4--named-topics)

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

By default, a node reuses one connection for every topic it has,
which keeps things simple. Call `node.set_multiplex(false)` if you'd
rather give each topic its own separate connection instead (some
tools/firewalls expect this) — see
[Step 12's benchmarks](#step-12--benchmarks) for the speed tradeoff
before reaching for it.

`rl_topic` (a small command-line tool included in the repo for
listing and inspecting topics — more in [Step 9](#using-rl_topic--listing-and-inspecting-topics))
works fine either way, with no extra setup needed.

### Pairing topics onto one port under `set_multiplex(false)`

`set_multiplex(false)` gives every topic its own separate connection —
useful sometimes, but wasteful if a node has many small, related
topics (say, a bunch of sensors) and doesn't want a separate connection
for every single one. `pair`/`pair_id` let you group a few topics back
onto one shared connection, while everything else on that node still
gets its own:

```cpp
node.set_multiplex(false);
node.advertise<Int16>("/room/light/1", false, false, /*pair=*/true, /*pair_id=*/1);
node.advertise<Int16>("/room/light/2", false, false, /*pair=*/true, /*pair_id=*/1);
node.advertise<Int16>("/room/light/3");  // unpaired -- still gets its own port
```

```python
node.set_multiplex(False)
node.advertise("/room/light/1", Int16, pair=True, pair_id=1)
node.advertise("/room/light/2", Int16, pair=True, pair_id=1)
node.advertise("/room/light/3", Int16)  # unpaired -- still gets its own port
```

Same `pair`/`pair_id` parameters exist on `subscribe`/`advertise_raw`/
`subscribe_raw` in both languages.

**Pairing is a purely local decision — the other side doesn't need to
know or match it.** A publisher grouping two topics onto one
connection was correctly received by a subscriber that kept those same
two topics on separate connections of its own — this choice never has
to be mirrored on both ends.

Re-declaring a topic under a different `pair_id` (or paired, then later
unpaired) raises/throws rather than silently rebinding it — almost
certainly a bug if it happens.

### `network_id` — domain isolation for Mode B (multicast)

Mode A (rlcore) naturally keeps separate deployments apart, since each
runs its own daemon. Mode B (multicast) doesn't do this by default —
every ReLink node listens on the same shared address, so two unrelated
projects running on the same network could accidentally hear each
other. `set_network_id(uint16_t)` fixes that by giving each deployment
its own private "channel" — call it before `use_multicast_discovery()`:

```cpp
node.set_network_id(42);
node.use_multicast_discovery();
```

```python
node.set_network_id(42)
node.use_multicast_discovery()
```

Nodes with different `network_id` values don't just ignore each other's
beacons — they join **different multicast group addresses *and*
different ports**, so a node on `network_id=42` never receives a single
byte from a `network_id=7` deployment on the same LAN, even accidentally.
`network_id=0` (the default — i.e. never calling `set_network_id()`)
behaves exactly like today, so existing single-domain setups see no
change. Why both the address *and* the port have to change together
(not just one) is a Linux kernel quirk explained in [Step 15 —
Technical deep dive](#step-15--technical-deep-dive).

### Review

- **Mode A (rlcore)** — one daemon, works everywhere; **Mode B (multicast)** — no daemon, needs multicast allowed.
- `set_multiplex(false)` moves a node from one shared socket to one-port-per-topic, ROS-style.
- `pair`/`pair_id` let specific topics share one port again under `set_multiplex(false)`, purely locally.
- `set_network_id(uint16_t)` isolates independent deployments on Mode B, like `ROS_DOMAIN_ID`.

Now that you understand discovery, let's look at naming topics.

---

## Step 4 — Named topics

**Description:** Using human-readable topic names instead of hand-assigned numeric ids.
**Tutorial Level:** Beginner

**◀ Previous:** [Step 3 — Pick a discovery mode](#step-3--pick-a-discovery-mode) &nbsp;|&nbsp; **Next ▶:** [Step 5 — Quick start: C++](#step-5--quick-start-c)

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
> Calling `publish()` with a string name looks up and re-hashes that
> string on every single call, which is fine once at startup but wastes
> time if you do it on every message in a fast loop. Resolve the name to
> its numeric id once with `topic_id_for()` (C++) / `_topic_id_for()`
> (Python), then publish by that id instead:
>
> ```cpp
> uint32_t topic_id = node.topic_id_for("/relink/temperature"); // once
> for (...) {
>     node.publish<Float32>(topic_id, Float32{ .data = reading }); // hot loop: numeric, no re-hash
> }
> ```
>
> Roughly twice as fast per call as publishing by name — see [Step 15 —
> Technical deep dive](#step-15--technical-deep-dive) for the measured
> numbers.

### Review

- A string topic name is hashed (FNV-1a) to the numeric id that actually goes on the wire.
- Both sides just need the same string — no shared header/constant required.
- Resolve a string topic once with `topic_id_for()`/`_topic_id_for()` and publish by id in a hot loop.

Now, let's write a complete node in C++.

---

## Step 5 — Quick start: C++

**Description:** A complete, runnable two-way pub/sub node in C++.
**Tutorial Level:** Beginner

**◀ Previous:** [Step 4 — Named topics](#step-4--named-topics) &nbsp;|&nbsp; **Next ▶:** [Step 6 — Quick start: Python](#step-6--quick-start-python)

This is a real, complete, runnable program — not a snippet. Every
ReLink node can publish and subscribe at the same time (there's no
separate "publisher node" vs. "subscriber node"), so this one file does
both: it broadcasts its own counted message on a timer, and prints
whatever any other copy of itself sends.

### The Code

Save the following as `pubsub.cpp`:

```cpp
// pubsub.cpp
#include "relink/relink.hpp"
#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>

struct Chatter { char data[128]; };
const char* TOPIC = "/relink/chatter";

int main() {
    RelinkNode node;
    node.use_multicast_discovery();   // zero setup -- no daemon to start first

    // Subscribe before advertising, so we don't miss early messages.
    // The callback fires on ReLink's own background thread whenever a
    // message arrives from any other node (never your own).
    node.subscribe<Chatter>(TOPIC, [](const Chatter& msg) {
        std::printf("received: %s\n", msg.data);
    });
    node.advertise<Chatter>(TOPIC);

    int count = 0;
    while (true) {                     // <- the real work happens here
        node.spin_once();              // services discovery -- call this every loop

        Chatter msg{};
        std::snprintf(msg.data, sizeof(msg.data), "hello world %d", count++);
        node.publish<Chatter>(TOPIC, msg);
        std::printf("sent:     %s\n", msg.data);

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}
```

### The Code Explained

Now, let's break down the code piece by piece.

```cpp
RelinkNode node;
node.use_multicast_discovery();
```

Every ReLink program starts by constructing a `RelinkNode` and picking
exactly one discovery mode (Step 3) before doing anything else.
`use_multicast_discovery()` is Mode B — no daemon to start first, which
is why it's the fastest way to get two nodes talking on a LAN.

```cpp
node.subscribe<Chatter>(TOPIC, [](const Chatter& msg) {
    std::printf("received: %s\n", msg.data);
});
node.advertise<Chatter>(TOPIC);
```

`subscribe<T>` registers a callback that fires on ReLink's own
background thread whenever a `Chatter` message arrives on `TOPIC` from
any other node — never from this node's own `publish()` calls.
`advertise<T>` declares that this node will publish `Chatter` on that
same topic. Subscribing before advertising means an early message from
a peer that started first is never missed.

```cpp
while (true) {
    node.spin_once();              // services discovery -- call this every loop
    ...
    node.publish<Chatter>(TOPIC, msg);
```

`spin_once()` is what actually drives the library — it services
discovery bookkeeping (beacon send/receive, peer-table updates) and
must be called on some regular cadence for the node to find peers and
keep receiving. `publish<T>()` sends `msg` to every peer currently
known for `TOPIC`; if no peer has been discovered yet, it's a no-op,
not an error.

### Building your node

```bash
g++ -std=c++17 -I cpp/include -pthread pubsub.cpp -o pubsub
```

Build against `cpp/include/` — header-only, no linking step
beyond `-pthread`. `node.set_rlcore.ip("10.0.0.5")` instead of
`use_multicast_discovery()` switches to Mode A (Step 9), needed if your
network blocks multicast or the two nodes aren't on the same LAN.

### Running it

Run the same binary in two terminals (or on two machines on the same
LAN) and watch them talk to each other:

```bash
./pubsub
```

You should see output interleaving both directions once the two copies
discover each other — this is real captured output from two instances
of the program above, not a mockup:

```
sent:     hello world 0
received: hello world 1
sent:     hello world 1
received: hello world 2
sent:     hello world 2
received: hello world 3
```

See `cpp/examples/` (Step 10) for more programs, including split
publisher/subscriber files and custom message types.

---

## Step 6 — Quick start: Python

**Description:** The same complete pub/sub node, in Python, wire-compatible with Step 5.
**Tutorial Level:** Beginner

**◀ Previous:** [Step 5 — Quick start: C++](#step-5--quick-start-c) &nbsp;|&nbsp; **Next ▶:** [Step 7 — Message types](#step-7--message-types)

The same idea, same wire format — a Python copy and a C++ copy of this
pattern talk to each other with zero changes on either side.

### The Code

Save the following as `pubsub.py`:

```python
#!/usr/bin/env python3
# pubsub.py
import ctypes
import time
from relink import RelinkNode

class Chatter(ctypes.Structure):
    _pack_ = 1
    _fields_ = [("data", ctypes.c_char * 128)]

TOPIC = "/relink/chatter"

def main():
    node = RelinkNode()
    node.use_multicast_discovery()   # zero setup -- no daemon to start first

    node.subscribe(TOPIC, Chatter, lambda msg: print("received:", msg.data.decode()))
    node.advertise(TOPIC, Chatter)

    count = 0
    while True:                      # <- the real work happens here
        node.spin_once()             # services discovery -- call this every loop

        msg = Chatter(data=f"hello world {count}".encode())
        node.publish(TOPIC, msg)
        print("sent:    ", msg.data.decode())
        count += 1

        time.sleep(0.5)

if __name__ == "__main__":
    main()
```

### The Code Explained

```python
class Chatter(ctypes.Structure):
    _pack_ = 1
    _fields_ = [("data", ctypes.c_char * 128)]
```

Every message type is a `ctypes.Structure` with `_pack_ = 1` set —
required so its in-memory layout is byte-exact, with no compiler
padding, matching the equivalent C++ `struct` field-for-field. ReLink
rejects a type at `subscribe()`/`advertise()` time if `_pack_ = 1` is
missing, rather than silently misreading the wire later.

```python
node = RelinkNode()
node.use_multicast_discovery()   # zero setup -- no daemon to start first
```

Same as the C++ side (Step 5): construct a node, pick exactly one
discovery mode before any other call.

```python
node.subscribe(TOPIC, Chatter, lambda msg: print("received:", msg.data.decode()))
node.advertise(TOPIC, Chatter)
```

`subscribe()` registers a callback for `TOPIC`, `advertise()` declares
this node publishes `Chatter` on it — subscribing first again avoids
missing an early message from a peer that started first.

```python
while True:
    node.spin_once()             # services discovery -- call this every loop
    ...
    node.publish(TOPIC, msg)
```

`spin_once()` must be called on some regular cadence to service
discovery; `publish()` sends to every currently-known peer for `TOPIC`
and is a no-op (not an error) if none have been discovered yet.

### Running it

No build step — pure stdlib (`ctypes` + `socket` + `struct`):

```bash
python3 pubsub.py        # run this in a second terminal too
```

This is real captured output from two instances of the program above:

```
sent:     hello world 0
received: hello world 1
sent:     hello world 1
received: hello world 2
sent:     hello world 2
received: hello world 3
```

`node.set_rlcore.ip("10.0.0.5")` instead of `use_multicast_discovery()`
switches to Mode A (Step 9). See `python/relink_py/README.md` and
`python/relink_py/examples/` (Step 10) for more programs, including custom
`ctypes.Structure` message types.

### Review

- `subscribe`/`advertise`/`publish`/`spin_once` is the same four-call API surface in both languages.
- Both the C++ and Python `pubsub` programs speak the exact same wire format and can talk to each other with zero changes.

Now that you can write a two-way node in either language, let's look at message types beyond the built-ins.

---

## Step 7 — Message types

**Description:** Built-in std_msgs-style types and defining your own trivially-copyable message struct.
**Tutorial Level:** Intermediate

**◀ Previous:** [Step 6 — Quick start: Python](#step-6--quick-start-python) &nbsp;|&nbsp; **Next ▶:** [Step 8 — Sending images](#step-8--sending-images)

`std_msgs`-style default types are built in (`Bool`, `Int32`, `Float32`,
...), plus **user-defined custom types**: any trivially-copyable struct
just works, no registration, no schema exchange, no codegen step.

### Built-in types

Every one of these wraps a single `data` field, byte-identical in both
languages (see `wire.hpp` / `wire.py`) — reach for a custom type (below)
only once one of these doesn't fit:

| Type | C++ | Python (`ctypes`) | Size |
|---|---|---|---|
| `Bool` | `uint8_t` (0/1) | `c_uint8` | 1 byte |
| `Byte` | `uint8_t` | `c_uint8` | 1 byte |
| `Char` | `uint8_t` | `c_uint8` | 1 byte |
| `Int8` | `int8_t` | `c_int8` | 1 byte |
| `Int16` | `int16_t` | `c_int16` | 2 bytes |
| `Int32` | `int32_t` | `c_int32` | 4 bytes |
| `Int64` | `int64_t` | `c_int64` | 8 bytes |
| `UInt8` | `uint8_t` | `c_uint8` | 1 byte |
| `UInt16` | `uint16_t` | `c_uint16` | 2 bytes |
| `UInt32` | `uint32_t` | `c_uint32` | 4 bytes |
| `UInt64` | `uint64_t` | `c_uint64` | 8 bytes |
| `Float32` | `float` | `c_float` | 4 bytes |
| `Float64` | `double` | `c_double` | 8 bytes |

[`builtin_types_pubsub`](#step-10--all-examples)
([C++](cpp/examples/builtin_types_pubsub.cpp) /
[Python](python/relink_py/examples/builtin_types_pubsub.py)) publishes
and subscribes every one of these on its own topic in a single
runnable file, and doubles as this table's live proof — verified
cross-language (C++ ↔ Python) as well as same-language.

### ROS-familiar composite types ("NoROSLib")

Beyond the primitives above, `standard_msgs.hpp`/`standard_msgs.py`
ship a much larger set of composite types, named and shaped after
their real ROS counterparts (`standard_msgs.hpp` itself calls this
"ROS/NoROSLib naming" — porting a ROS node's message usage to ReLink
is meant to be a rename, not a redesign). Every one is built from
exactly the same rule as any other custom type: a trivially-copyable,
fixed-layout struct — the deliberate departures from ROS's actual wire
format (fixed-size strings instead of dynamic ones, `float32` instead
of `float64` for `geometry_msgs`, fixed-capacity arrays instead of
unbounded ones) are documented at the top of `standard_msgs.hpp`.

| Package | Types |
|---|---|
| `std_msgs` (19) | `Bool`, `Byte`, `Char`, `ColorRGBA`, `Duration`, `Empty`, `Float32`, `Float64`, `Header`, `Int8`, `Int16`, `Int32`, `Int64`, `String`, `Time`, `UInt8`, `UInt16`, `UInt32`, `UInt64` |
| `geometry_msgs` (16) | `Accel`, `Point`, `Point32`, `Polygon`, `Pose`, `PoseArray`, `PoseStamped`, `PoseWithCovariance`, `Quaternion`, `Transform`, `TransformStamped`, `Twist`, `TwistStamped`, `TwistWithCovariance`, `Vector3`, `Wrench` |
| `sensor_msgs` (14) | `CameraInfo`, `CompressedImage`*, `Image`*, `Imu`, `JointState`, `LaserScan`, `MagneticField`, `NavSatFix`, `NavSatStatus`, `PointCloud2`, `PointField`, `Range`, `RegionOfInterest`, `Temperature` |
| `nav_msgs` (5) | `GridCells`, `MapMetaData`, `OccupancyGrid`, `Odometry`, `Path` |
| `diagnostic_msgs` (3) | `DiagnosticArray`, `DiagnosticStatus`, `KeyValue` |
| `trajectory_msgs` (4) | `JointTrajectory`, `JointTrajectoryPoint`, `MultiDOFJointTrajectory`, `MultiDOFJointTrajectoryPoint` |
| `actionlib_msgs` (3) | `GoalID`, `GoalStatus`, `GoalStatusArray` |

\* `Image`/`CompressedImage` map to ReLink's `ImageChunk` — a chunked
large-blob type that goes through `advertise_image`/`publish_image`/
`subscribe_image` instead of plain `advertise`/`publish`/`subscribe`,
since a full image doesn't fit one UDP datagram (Step 8). Every other
type in the table above is a normal, single-datagram message.

[`standard_msgs_pubsub`](#step-10--all-examples)
([C++](cpp/examples/standard_msgs_pubsub.cpp) /
[Python](python/relink_py/examples/standard_msgs_pubsub.py)) publishes
and subscribes every one of these 49 types (everything except
`Image`/`CompressedImage`) on its own topic in a single runnable file —
a live reference for the whole table, verified cross-language
(C++ ↔ Python) as well as same-language. Each package also has its own
standalone example (Step 10) if you only want to see one package's
shapes without the other six:

| Package | Example |
|---|---|
| `std_msgs` (+ `*MultiArray` family) | [`std_msgs_pubsub`](#step-10--all-examples) ([C++](cpp/examples/std_msgs_pubsub.cpp) / [Python](python/relink_py/examples/std_msgs_pubsub.py)) |
| `geometry_msgs` | [`geometry_msgs_pubsub`](#step-10--all-examples) ([C++](cpp/examples/geometry_msgs_pubsub.cpp) / [Python](python/relink_py/examples/geometry_msgs_pubsub.py)) |
| `sensor_msgs` | [`sensor_msgs_pubsub`](#step-10--all-examples) ([C++](cpp/examples/sensor_msgs_pubsub.cpp) / [Python](python/relink_py/examples/sensor_msgs_pubsub.py)) |
| `nav_msgs` | [`nav_msgs_pubsub`](#step-10--all-examples) ([C++](cpp/examples/nav_msgs_pubsub.cpp) / [Python](python/relink_py/examples/nav_msgs_pubsub.py)) |
| `diagnostic_msgs` | [`diagnostic_msgs_pubsub`](#step-10--all-examples) ([C++](cpp/examples/diagnostic_msgs_pubsub.cpp) / [Python](python/relink_py/examples/diagnostic_msgs_pubsub.py)) |
| `trajectory_msgs` | [`trajectory_msgs_pubsub`](#step-10--all-examples) ([C++](cpp/examples/trajectory_msgs_pubsub.cpp) / [Python](python/relink_py/examples/trajectory_msgs_pubsub.py)) |
| `actionlib_msgs` | [`actionlib_msgs_pubsub`](#step-10--all-examples) ([C++](cpp/examples/actionlib_msgs_pubsub.cpp) / [Python](python/relink_py/examples/actionlib_msgs_pubsub.py)) |

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

See [`custom_types_pubsub`](#step-10--all-examples)
([C++](cpp/examples/custom_types_pubsub.cpp) /
[Python](python/relink_py/examples/custom_types_pubsub.py)) for a full
runnable version of this pattern — a built-in type and two custom
types (one plain, one with a fixed-size array field) on the same node.

Both sides must independently define the identical byte layout — ReLink
does no schema negotiation between nodes, same as ROS relies on both
sides being built against the same generated message header.

### Review

- Built-in `std_msgs`-style types cover the common cases; a custom type is just a packed struct.
- No codegen, no `.msg` file, no schema exchange — both sides just need to define the same layout independently.

Now that you know how to define a message type, let's send something bigger than one UDP datagram.

---

## Step 8 — Sending images

**Description:** Sending payloads larger than one UDP datagram with the built-in Image type.
**Tutorial Level:** Intermediate

**◀ Previous:** [Step 7 — Message types](#step-7--message-types) &nbsp;|&nbsp; **Next ▶:** [Step 9 — Running the rlcore daemon](#step-9--running-the-rlcore-daemon)

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

### Review

- `advertise_image`/`publish_image`/`subscribe_image` chunk and reassemble automatically — no hand-rolled chunking.
- A larger socket buffer trades drops for latency on short bursts; it doesn't fix a slow subscriber callback.
- Prefer `image_compressed` over raw frames for anything real-time.

Now, let's look at running the rlcore daemon for Mode A discovery.

---

## Step 9 — Running the rlcore daemon

**Description:** Building, running, and using relink-rlcore for Mode A discovery, including NAT traversal and rl_topic.
**Tutorial Level:** Intermediate

**◀ Previous:** [Step 8 — Sending images](#step-8--sending-images) &nbsp;|&nbsp; **Next ▶:** [Step 10 — All examples](#step-10--all-examples)

Only needed for Mode A discovery (Step 3) — skip this whole section if
you're using `use_multicast_discovery()` (Mode B).

### What `relink-rlcore` actually is

One small standalone daemon (no dependencies beyond the C++ standard
library) that every node on your network registers with once at
startup. It has no idea what your messages mean and never carries your
actual pub/sub traffic — its only job is answering "who else is out
there for topic X, and how do I reach them." See [rlcore in plain
terms](#rlcore-in-plain-terms) below for the non-jargon version.

### Building it

```bash
# Option A: plain g++, one file, no build system needed
g++ -std=c++17 -O2 -pthread cpp/rlcore/relink_rlcore.cpp -o relink-rlcore

# Option B: the CMake project under cpp/rlcore/ (also builds relink-relay,
# see "AF_XDP" below) — same result, useful if you want it alongside
# relink-relay in one build directory
cd cpp/rlcore && cmake -B build . && cmake --build build
# binaries land in cpp/rlcore/build/relink-rlcore and cpp/rlcore/build/relink-relay

# Python — no build step at all, pure stdlib
python3 python/rlcore/relink_rlcore.py --port 8445 [--nat]
```

### Running it

```bash
./relink-rlcore --port 8445          # plain mode: LAN-only discovery
./relink-rlcore --port 8445 --nat    # adds cross-network / NAT punching, see below
```

Either the C++ or Python build works with either language's client
nodes — same wire protocol, pick whichever is more convenient to run
where you're running it. Only one instance needs to run, reachable by
every node that will use it; nodes never talk to each other through it
after the initial handshake.

### `--nat`: reaching nodes behind a router/firewall

Without `--nat`, rlcore only works for nodes on the same LAN — it
hands out each node's *self-reported* address, which is meaningless to
a peer on a different network (it's usually a private `192.168.x.x`
address). `--nat` changes what rlcore hands out: instead of trusting
what a node says its address is, it uses the address the node's
registration packet actually *arrived from* — the real, internet-
facing address your router assigned that connection. Paired with each
node automatically sending a burst of small "punch" packets to every
peer it's told about, this is standard UDP hole punching: both sides'
routers open an outbound mapping toward each other at roughly the same
time, and direct traffic gets through afterward without either side
needing port forwarding configured.

```bash
./relink-rlcore --port 8445 --nat
```

```cpp
// Both nodes just point at the same rlcore instance -- nothing else
// changes in your pub/sub code:
node.set_rlcore.ip("203.0.113.10");   // rlcore's public IP
```

This works reliably when at least one side has a NAT type that allows
it (most home routers do). Some NAT types (notably "symmetric" NAT,
common on mobile carriers and some corporate networks) structurally
cannot be punched through — no software workaround exists for that,
only a relay in the middle. That's what `relink-relay` is for:

```cpp
node.set_rlcore.ip("203.0.113.10");
node.set_relay("203.0.113.10");   // same machine works fine; a relay just needs to be reachable by both sides
```

With both lines set, a node always tries the direct punched path
first and *also* sends every message through the relay as a backup —
whichever arrives, arrives; the relay copy is silently deduplicated if
the direct one also got through. You don't have to detect or configure
which case you're in.

```bash
# Build + run the relay (plain sockets, no AF_XDP -- see below)
g++ -std=c++17 -O2 -I cpp/include -pthread cpp/rlcore/relink_relay.cpp -o relink-relay
./relink-relay [port]   # default 8446

# Python
python3 python/rlcore/relink_relay.py [port]
```

**Stress-tested**: 30 concurrent nodes registering, punching, and
publishing through the same `relink-rlcore --nat` instance at once —
30/30 succeeded, 900/900 messages delivered, 0 failures (numbers below
in Step 12). Step 13 covers the full technical mechanism (background
re-punch, multiplexed-vs-per-topic-port routing, exactly what a NAT
type can and can't be punched through).

### `--encrypt-key`: encrypting the registration handshake

By default, the RegisterRequest/RegisterAck exchange with rlcore is
plaintext UDP — anyone on the same network segment (or in the NAT-
traversal case, the same relay) could read who's registering which
topics with which address, or spoof a reply. `--encrypt-key` seals that
exchange with AES-256-GCM under a pre-shared key, so it's both
confidential and authenticated (a wrong or missing key gets the packet
dropped, not silently accepted). This only covers the signaling
handshake with rlcore, not the pub/sub data path itself, which stays
plain UDP multicast/unicast as documented elsewhere in this README.

**Why the data path itself isn't encrypted**: this is deliberate, not
an oversight. Payload encryption would put AES-GCM (key setup, nonce
generation, tag computation) on the hot path of *every single message*
`publish()` sends — the same hot path Step 4/Step 15 already measure in
nanoseconds and optimize for zero allocation, since ReLink's whole
value proposition against a general framework like ROS 2/DDS is low
per-message overhead. Registration happens once at startup plus one
re-registration packet every ~0.3s per node — cheap to encrypt
regardless of algorithm cost. Payload encryption doesn't have that
luxury: it runs at whatever rate you're publishing (this project's own
benchmarks push 1000+ Hz). If your deployment needs the data path
encrypted too (e.g. untrusted network segments), that's a real gap
today — put ReLink traffic inside a WireGuard/IPsec tunnel at the OS
level instead, which gets you confidentiality without taxing every
`publish()` call.

Generate a key once, then give the same key to rlcore and to every
node that talks to it:

```bash
./relink-rlcore --generate-key
# ef78d9acb11a87844e705a07ae66ddd7f0124c28899da2fa56062a6367e42e3d
#   (prints one key and exits -- doesn't start the daemon)

./relink-rlcore --port 8445 --encrypt-key ef78d9acb11a87844e705a07ae66ddd7f0124c28899da2fa56062a6367e42e3d
```

```cpp
node.set_rlcore.ip("203.0.113.10");
node.set_rlcore.setEncryptKey("ef78d9acb11a87844e705a07ae66ddd7f0124c28899da2fa56062a6367e42e3d");
```

#### Step-by-step setup

1. **Build `relink-rlcore`** if you haven't already (Step 9's
   "Building it" section above) — encryption support is compiled in by
   default, no extra flag needed.

2. **Generate one key** — run this once, anywhere, not once per node:

   ```bash
   ./relink-rlcore --generate-key
   # ef78d9acb11a87844e705a07ae66ddd7f0124c28899da2fa56062a6367e42e3d
   ```

   Treat this string like a password: whoever has it can register fake
   peers with your rlcore, or read who's registering what. Don't commit
   it to source control or paste it somewhere public.

3. **Start rlcore with that key**:

   ```bash
   ./relink-rlcore --port 8445 --encrypt-key ef78d9acb11a87844e705a07ae66ddd7f0124c28899da2fa56062a6367e42e3d
   ```

4. **Give the SAME key to every node** that will register with this
   rlcore, right after pointing it at rlcore's address — C++ and
   Python nodes can mix freely against the same encrypted rlcore, in
   either language, since both sides implement the identical
   AES-256-GCM wire format:

   ```cpp
   node.set_rlcore.ip("203.0.113.10");
   node.set_rlcore.setEncryptKey("ef78d9acb11a87844e705a07ae66ddd7f0124c28899da2fa56062a6367e42e3d");
   ```

   ```python
   node.set_rlcore.ip("203.0.113.10")
   node.set_rlcore.set_encrypt_key("ef78d9acb11a87844e705a07ae66ddd7f0124c28899da2fa56062a6367e42e3d")
   ```

   Order doesn't matter between `.ip(...)` and `setEncryptKey(...)`/
   `set_encrypt_key(...)`, but both must be called before
   `node.start()`/the first `advertise()`/`subscribe()` call that
   triggers registration. `relink-rlcore.py --generate-key` and
   `--encrypt-key` work identically to the C++ daemon's flags shown
   above, if you're running the Python daemon instead:

   ```bash
   python3 python/rlcore/relink_rlcore.py --port 8445 --encrypt-key ef78d9acb11a87844e705a07ae66ddd7f0124c28899da2fa56062a6367e42e3d
   ```

5. **Verify it worked**: start rlcore first, then a node, and watch
   rlcore's own stdout. A successful encrypted registration prints the
   normal `relink-rlcore: registered <ip>:<port> (N topics)...` line —
   there's no separate "encrypted" indicator, because from rlcore's
   side a correctly-decrypted request looks identical to always. If the
   key is wrong or missing on one side, rlcore instead prints
   `dropped RegisterRequest that failed to decrypt` for every attempt,
   and the node's own stderr shows it retrying and eventually giving up
   (`register_with_rlcore: giving up after 3 attempts`).

#### Troubleshooting

| Symptom | Likely cause |
|---|---|
| Node registration times out only after adding `--encrypt-key` | Node is missing `setEncryptKey(...)`, or the hex string doesn't match rlcore's byte-for-byte (copy/paste error, trailing whitespace/newline) |
| `setEncryptKey` throws at startup | The string isn't exactly 64 hex characters — regenerate with `--generate-key` rather than hand-typing one |
| Some nodes register fine, others don't, same rlcore | Only some nodes were updated with the new key after a key rotation — every node must be updated at the same time you restart rlcore with the new key, since there's no "accept either key" transition mode |

#### C++ and Python interop

Both languages implement the same AES-256-GCM wire format from
scratch — `cpp/include/relink/crypto.hpp` (C++) and
`python/relink_py/relink/crypto.py` (Python), neither depending on a
third-party crypto library (see "Implementation notes" below). A
message sealed by one is verified to decrypt correctly with the other,
and this was tested in both daemon/client combinations: a Python node
registering with a C++ `relink-rlcore --encrypt-key ...`, and a C++
node registering with a Python `relink_rlcore.py --encrypt-key ...`.
There's no restriction on mixing languages in an encrypted fleet.

Python's implementation is pure-stdlib (no `pycryptodome`/
`cryptography` install needed, matching this project's "Python needs
no build step at all" story) but noticeably slower per call than the
C++ side — irrelevant here since, same as the C++ side, it only runs
on registration/re-registration (once at startup, then every ~0.3s per
node), never on the `publish()` hot path.

#### Implementation notes

The AES-256-GCM implementation is self-contained (no OpenSSL/libcrypto
dependency) rather than linking a system crypto library, so it doesn't
affect the Windows/macOS build steps above — MinGW has no standard
OpenSSL package, and depending on one would have silently broken the
Windows cross-compile. Verified byte-for-byte interoperable with
OpenSSL's own AES-256-GCM (encrypt with one, decrypt with the other, in
both directions) during development, plus tamper detection (flipping
one ciphertext byte is rejected) and a live encrypted registration
exchange between a native Linux node and `relink-rlcore.exe` running
under Wine.

### AF_XDP — optional, and fully detachable

`relink-relay` has an optional "fast mode" (`-DRELINK_ENABLE_XDP`) that
makes it a bit faster (~400-550 µs per round trip, Step 12 has the
numbers) at the cost of needing extra build tools and root access. It's
off by default and safe to ignore — plain mode works everywhere with no
extra setup:

```bash
cd rlcore
cmake -B build .                       # plain relay -- no extra dependencies
cmake -B build . -DRELINK_ENABLE_XDP=ON  # same relay, faster, needs libbpf + clang
cmake --build build
```

If fast mode is turned on but the machine can't actually support it,
`relink-relay` notices at startup and quietly falls back to plain mode
— it never refuses to run. See [Step 15 — Technical deep
dive](#step-15--technical-deep-dive) for how it actually works under
the hood.

### rlcore in plain terms

If the discovery-mode names (Mode A/B) feel abstract, here's the same
thing without the jargon: `relink-rlcore` is one small program you run
once, on one machine both your other programs can reach — think of it
as a phone book. Every node calls it once on startup to say "here I
am, and here's what I publish," and rlcore tells each node how to
reach the others directly. After that, rlcore is out of the way —
actual messages never pass through it, only the one-time introduction
does.

A complete two-machine example — a temperature sensor on one machine, a
logger on another, found via rlcore:

```bash
# 1. On any reachable machine (or your own laptop), start the phone book once:
./relink-rlcore --port 8445
```

```cpp
// 2. sensor.cpp — publishes a reading once a second
#include "relink/relink.hpp"
#include <unistd.h>

int main() {
    RelinkNode node;
    node.set_rlcore.ip("10.0.0.5");   // the machine running relink-rlcore
    node.advertise<Float32>("/temperature");
    while (true) {
        node.publish<Float32>("/temperature", Float32{ .data = 36.6f });
        sleep(1);
    }
}
```

```python
# 3. logger.py — prints every reading it receives
from relink import RelinkNode, Float32

node = RelinkNode()
node.set_rlcore.ip("10.0.0.5")   # same phone book as the sensor
node.subscribe("/temperature", Float32, lambda msg: print("got:", msg.data))
node.spin()
```

Run all three (in any order — nodes retry until rlcore is reachable),
and `logger.py` starts printing readings from `sensor.cpp` a moment
later, even though they're two different languages on two different
machines. That's the whole point: rlcore just handles the "how do I
find you" problem so your actual code doesn't have to.

### Using `rl_topic` — listing and inspecting topics

`rl_topic` (`python/rl_topic.py` / `cpp/tools/rl_topic.cpp`, built the
same way as any other example in [Step 10](#step-10--all-examples)) is a
`rostopic`-style CLI for asking "what topics exist right now, and who's
using them" without writing any code. `list` and `info` are the two most
common subcommands:

```bash
# Point it at your rlcore daemon (same ip:port your nodes use):
python3 rl_topic.py list --rlcore-ip 10.0.0.5

# every topic id currently registered with rlcore, plus its name if any
# node has resolved one for it (see Step 4 — a purely numeric topic
# still shows up, just as "(unnamed)"):
#   377369340   /relink/temperature
#   2779401242  (unnamed)
```

```bash
python3 rl_topic.py info /relink/temperature --rlcore-ip 10.0.0.5
# Topic id : 377369340
# Name     : /relink/temperature
# Source   : rlcore 10.0.0.5
# Publishers  (1):
#   10.0.0.5:53211
# Subscribers (1):
#   10.0.0.7:41830
```

`info` shows real **p2p connection details** — which `ip:port` is
publishing and which is subscribing that topic, live, straight from
rlcore's own bookkeeping (not from probing the nodes themselves). `info`
also accepts a numeric topic id directly (`rl_topic.py info 377369340`),
which works even for a topic that was never given a string name.

A few things worth knowing:

- **`--rlcore-ip`/`--rlcore-port`** select Mode A (rlcore) instead of
  broadcasting to the Mode B multicast group — required for `info`'s
  publisher/subscriber breakdown, since only rlcore keeps that central
  table; multicast mode has no central registry to ask (each node only
  knows about itself, per [Step 0](#step-0--what-relink-actually-is)).
  Once given, both are remembered in `~/.cache/relink/conf.bin`, so a
  later run without `--rlcore-ip` reuses the same rlcore automatically —
  topic data itself is never cached, since a stale topic/peer list is
  actively misleading (see the note printed to stderr when this kicks in).
- **Freshness**: rlcore expires a registration (and its publisher/
  subscriber role) if that node hasn't re-registered in the last few
  seconds — closing a node makes it disappear from `list`/`info` shortly
  after, it doesn't linger forever.
- **`echo`** decodes payloads too, not just hex: `--type Float32` (or
  any other built-in type from `standard_msgs.py`/`std_msgs`/
  `geometry_msgs`/etc. — `Imu`, `Pose`, ...) or `--msg path/to/custom.msg`
  (your own schema — see `relink_py/relink/msg_schema.py` for the
  `.msg` file format) pretty-prints field values; `--hex` always forces
  raw bytes regardless. With neither flag, it prints hex by default,
  same as before this existed — ReLink has no wire-level type registry
  to guess a shape from automatically.
- **`hz`/`bw`/`pub`** round out the CLI (rate, bandwidth, and manual
  publish by `--hex`/`--text`) — run `rl_topic.py <subcommand> --help`
  for the full flag list on any of them.

### Review

- `relink-rlcore` is a rendezvous point only — actual message traffic never passes through it.
- `--nat` enables cross-network discovery via hole punching; `relink-relay` is the fallback for NAT types punching can't cross.
- `rl_topic` (`list`/`info`/`hz`/`bw`/`echo`/`pub`) is the `rostopic`-equivalent CLI for inspecting a running system without writing code.

Now that discovery and daemons are covered, let's tour every example program in the repo.

---

## Step 10 — All examples

**Description:** A tour of every example program shipped in cpp/examples/ and python/relink_py/examples/.
**Tutorial Level:** Beginner

**◀ Previous:** [Step 9 — Running the rlcore daemon](#step-9--running-the-rlcore-daemon) &nbsp;|&nbsp; **Next ▶:** [Step 11 — Testing](#step-11--testing)

Every example is a complete, runnable program, not a snippet — each one
exists in both C++ (`cpp/examples/`) and Python (`python/relink_py/examples/`).

| Example | What it shows | Run it |
|---|---|---|
| **`hello_relink`** | The simplest possible node — one file, no arguments, both publisher and subscriber at once. Start here. | `./hello_relink` / `python3 hello_relink.py` (run twice) |
| **`talker` + `listener`** | Publisher and subscriber split into two separate files/roles, the classic ROS-tutorial shape. | `./listener` then `./talker` |
| **`pubsub`** | `talker` + `listener` combined into one file/process — both roles at once, interoperable with either standalone binary above. | `./pubsub` (run twice, or against `talker`/`listener`) |
| **`rlcore_pubsub`** | Mode A (daemon) discovery, a custom message type alongside a default type, one process as publisher and one as subscriber. | `./rlcore_pubsub pub <daemon_ip>` and `... sub <daemon_ip>`, daemon already running |
| **`multicast_pubsub`** | Same pub/sub shape as `rlcore_pubsub`, but Mode B — shows the two discovery modes are interchangeable from the application's point of view. | `./multicast_pubsub pub` and `... sub` |
| **`builtin_types_pubsub`** | Every built-in primitive message type ReLink ships (Step 7's built-in types table), one topic per type, in a single runnable file — a live reference list, not just documentation. | `./builtin_types_pubsub` (run twice, or against the Python copy) |
| **`standard_msgs_pubsub`** | Every ROS-familiar composite type ReLink ships (Step 7's `std_msgs`/`geometry_msgs`/`sensor_msgs`/`nav_msgs`/`diagnostic_msgs`/`trajectory_msgs`/`actionlib_msgs` table) — all 49 non-image types, one topic per type, in a single runnable file. | `./standard_msgs_pubsub` (run twice, or against the Python copy) |
| **`std_msgs_pubsub`** | Just the `std_msgs` package split out of `standard_msgs_pubsub` above: `Empty`, `Time`, `Duration`, `ColorRGBA`, `Header`, `String`, plus the `*MultiArray` family (`Byte`/`Int8`/`Int16`/`Int32`/`Int64`/`UInt8`/`UInt16`/`UInt32`/`UInt64`/`Float32`/`Float64MultiArray` — 11 types, user-defined here since `standard_msgs.hpp`/`.py` only document the underlying layout, not ready-made structs). | `./std_msgs_pubsub` (run twice, or against the Python copy) |
| **`geometry_msgs_pubsub`** | Just the `geometry_msgs` package: `Vector3`, `Point`, `Point32`, `Quaternion`, `Pose`, `Twist`, `Accel`, `Wrench`, `PoseStamped`, `TwistStamped`, `Transform`, `TransformStamped`, `PoseWithCovariance`, `TwistWithCovariance`, `PoseArray`, `Polygon`. | `./geometry_msgs_pubsub` (run twice, or against the Python copy) |
| **`sensor_msgs_pubsub`** | Just the `sensor_msgs` package (everything except `Image`/`CompressedImage`, which need `advertise_image`, see `camera_stream` below): `Imu`, `NavSatStatus`, `NavSatFix`, `MagneticField`, `Temperature`, `Range`, `RegionOfInterest`, `CameraInfo`, `PointField`, `PointCloud2`, `LaserScan`, `JointState`. | `./sensor_msgs_pubsub` (run twice, or against the Python copy) |
| **`nav_msgs_pubsub`** | Just the `nav_msgs` package: `Odometry`, `MapMetaData`, `Path`, `OccupancyGrid`, `GridCells`. | `./nav_msgs_pubsub` (run twice, or against the Python copy) |
| **`diagnostic_msgs_pubsub`** | Just the `diagnostic_msgs` package: `KeyValue`, `DiagnosticStatus`, `DiagnosticArray`. | `./diagnostic_msgs_pubsub` (run twice, or against the Python copy) |
| **`trajectory_msgs_pubsub`** | Just the `trajectory_msgs` package: `JointTrajectoryPoint`, `JointTrajectory`, `MultiDOFJointTrajectoryPoint`, `MultiDOFJointTrajectory`. | `./trajectory_msgs_pubsub` (run twice, or against the Python copy) |
| **`actionlib_msgs_pubsub`** | Just the `actionlib_msgs` package: `GoalID`, `GoalStatus`, `GoalStatusArray`. | `./actionlib_msgs_pubsub` (run twice, or against the Python copy) |
| **`custom_types_pubsub`** | "Any message type" made concrete: one node publishing/subscribing a built-in type (`Bool`) alongside two user-defined custom types at once — a small struct (`Pose2D`) and a struct containing fixed-size arrays (`Waypoints`). Verified interoperable both same-language and cross-language (C++ ↔ Python). | `./custom_types_pubsub` (run twice, or against the Python copy) |
| **`camera_stream`** | A real webcam streamed over ReLink two ways at once (`image_raw`, `image_compressed`) using the built-in `Image` type. **Requires OpenCV**, installed yourself — not a ReLink dependency. | `./camera_stream pub` and `... sub` |

There's also a performance test harness (`cpp/tools/relink_benchmark.cpp`)
used to produce the numbers in Step 12 — worth reading once you're
comfortable with the basics, not a starting point.

---

## Step 11 — Testing

**Description:** Running the unit and two-process correctness/interop test suites.
**Tutorial Level:** Intermediate

**◀ Previous:** [Step 10 — All examples](#step-10--all-examples) &nbsp;|&nbsp; **Next ▶:** [Step 12 — Benchmarks](#step-12--benchmarks)

```bash
# C++ (each test is a standalone binary)
g++ -std=c++17 -I cpp/include -pthread cpp/tests/test_wire.cpp -o test_wire && ./test_wire

# Python
python3 python/relink_py/tests/test_relink.py
```

Every layer (wire format, ring buffer, framing, discovery, named-topic
hashing, the public API's error paths) has a plain-assert test suite in
both languages, plus two-process correctness tests and cross-language
interop tests (`cpp/tests/two_process_{pub,sub}.cpp` /
`python/relink_py/tests/two_process_{pub,sub}.py`) proving the C++ and Python
nodes actually interoperate over real sockets, not just in theory.

---

## Step 12 — Benchmarks

**Description:** Measured latency/throughput numbers, head-to-head against ROS2, and the AF_XDP fast path.
**Tutorial Level:** Advanced

**◀ Previous:** [Step 11 — Testing](#step-11--testing) &nbsp;|&nbsp; **Next ▶:** [Step 13 — NAT traversal (cross-network nodes)](#step-13--nat-traversal-cross-network-nodes)

**In plain terms**: using the C++ version, a message sent by one
program typically arrives at the other in well under a millisecond —
around 40-170 microseconds on average depending on the setup, which is
1,000x faster than the blink of an eye. Compared to ROS2 (the most
common alternative), ReLink was 2-3x faster on average and had a much
more consistent worst case — ROS2's occasional slow message (up to
3.7ms) was almost 10x slower than ReLink's. On the relay path
specifically, ReLink handled a sustained 10,000 messages per second
with zero messages lost and typical delivery in under 100
microseconds (details in [the AF_XDP fast path
section](#af_xdp-fast-path) below).

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

### AF_XDP fast path

`cpp/rlcore/relink_relay.cpp`, built with `-DRELINK_ENABLE_XDP` —
an opt-in build flag that has the relay bypass the kernel's normal UDP
receive path for matched traffic via a native/driver-mode XDP program +
AF_XDP socket (falls back to a plain socket automatically if the
kernel/driver/toolchain don't support it — see `cpp/rlcore/xdp/relay_xdp.hpp`).
All results below are against a real production VPS (Ubuntu 20.04,
virtio_net NIC, libbpf 0.5.0), not loopback.

*Round-trip latency, AF_XDP vs. plain socket, identical relay code and
network path, only the RX mechanism differs* (WAN: dev laptop ↔ VPS,
500 samples, two relay hops per sample — pinger → relay → responder →
relay → pinger):

| Path | min | p50 | p90 | p99 | max | avg |
|---|---|---|---|---|---|---|
| AF_XDP | 4488.7 µs | 4980.4 µs | 6169.1 µs | 8520.7 µs | 13226.6 µs | 5308.7 µs |
| Plain socket | 4823.4 µs | 5409.3 µs | 6645.5 µs | 9074.3 µs | 14283.7 µs | 5735.6 µs |

AF_XDP is consistently ~400-550 µs faster per round trip (2 hops) — a
real, measurable win, small relative to WAN RTT here but a fixed
per-hop saving that matters proportionally more on a LAN or between
geographically close peers.

*Single-topic sustained throughput* (paced publish, one pub + one sub,
loss and duplicates measured, AF_XDP relay):

| Publisher/subscriber location | Rate | Messages | Loss | Duplicates |
|---|---|---|---|---|
| Dev laptop (WAN, real home uplink) | 500 msg/s | 5,000 | 0% | 0 |
| Dev laptop (WAN, real home uplink) | 2,000 msg/s | 10,000 | 0% | 0 |
| Dev laptop (WAN, real home uplink) | 5,000 msg/s | 20,000 | 0% | 0 |
| Dev laptop (WAN, real home uplink) | 10,000 msg/s | 30,000 | 18-44%\* | 0 |
| **VPS itself** (both pub and sub on the relay's own box, via its real NIC — not loopback) | 10,000 msg/s | 30,000 | **0%** | 0 |

\* At 10,000 msg/s from the dev laptop, loss appeared on two separate
runs (44.3% and 18.5%) — but the VPS's own NIC redirect counters
(`ethtool -S eth0`, `rx_queue_0_xdp_redirects`) showed an *exact*
packet-count match both times (e.g. 30,002 in for 30,000 data +
2 registration packets, zero new `rx_queue_0_xdp_drops`), proving the
relay received and processed 100% of traffic with zero server-side
loss. Re-running the identical test with both publisher and subscriber
on the VPS itself (removing the dev laptop's home network from the
path entirely) reproduced 0% loss with sub-100 µs latency
(min=18.0 µs, p50=41.0 µs, p90=70.0 µs, p99=159.0 µs, avg=55.8 µs).
The loss at 10,000 msg/s from the laptop is the laptop's home
internet uplink saturating under a sustained ~10k pps flow, not a
defect in the relay or its AF_XDP path.

**`relink-rlcore --nat` under concurrent load** — real cross-network
nodes (a dev laptop behind a home NAT ↔ the VPS), each topic
independently registering with rlcore, learning its peer's NAT-mapped
address, attempting a direct punch, and falling back to the relay
(this laptop's NAT type structurally blocks the specific punch
pattern here, confirmed separately — see Step 13), fired concurrently:

| Concurrent topics | Messages/topic | Total messages | Passed | Failed |
|---|---|---|---|---|
| 10 | 20 | 200 | 10/10 | 0 |
| 30 | 30 | 900 | 30/30 | 0 |

Every publisher resolved exactly one peer and every subscriber
received 100% of its messages with zero duplicates, across both runs —
rlcore's registration/rendezvous logic and the relay-fallback path
both held up cleanly with no cross-talk between concurrently-running
topics.

### Image / video streaming (real webcam)

All tests below used a real `/dev/video0` webcam and the actual
`camera_stream` example (Step 10), not synthetic data.

**Same-machine, both resolutions the test camera actually supports**
(everything else this camera claims to support returned an empty
frame — a driver limitation, not a ReLink one):

| Resolution | Frames sent | Delivered (raw) | Delivered (compressed) |
|---|---|---|---|
| 320×240 | 46 | 46/46 (100%) | 46/46 (100%) |
| 640×480 | 42 | 42/42 (100%) | 42/42 (100%) |

**Cross-network, real NAT hole punching** (laptop webcam behind a home
NAT → VPS, via `relink-rlcore --nat` — `Image` has **no relay
fallback** at all, see Step 13, so this is testing the direct punched
path in isolation with nothing to fall back to). Publishing raw and
compressed together, at increasing target FPS:

| Target FPS | Actual FPS (camera-limited) | Raw delivered | Compressed delivered |
|---|---|---|---|
| 5 | 5.0 | 39/40 (97.5%) | 39/40 (97.5%) |
| 10 | 10.0 | 21/80 (26%) | 60/80 (75%) |
| 15 | 8.0 | 1/65 (1.5%) | 31/65 (48%) |
| 30 | 6.1 | 0/49 (0%) | 20/49 (41%) |

Two findings here: this webcam physically tops out around 6-8 FPS once
JPEG encode + a several-hundred-chunk raw frame's worth of `sendto()`
calls are in the loop every frame — and **raw collapses far faster
than compressed as load increases**, exactly matching Step 8's
warning (one dropped chunk drops the whole frame; raw is ~166 chunks
at this resolution, compressed is 2-6).

**Isolating the actual bottleneck**: capturing frames alone hit a true
30 FPS, and JPEG-encoding them also stayed at 30 FPS — the slowdown
above was specifically the *raw* topic's per-frame chunk-send volume,
not the camera or the encoder. Dropping the raw topic and publishing
**compressed only**:

| Target FPS | Actual FPS | Delivered |
|---|---|---|
| 30 | 29.8 | 229/239 (95.8%) |

A real webcam, real cross-NAT delivery, at a genuine ~30 FPS with
~96% of frames arriving intact — `image_compressed` is the one to use
for anything resembling live video; raw is fine for occasional
snapshots or a LAN with no NAT/relay concerns.

(One caveat on the FPS-ladder table above: those four runs reused the
same topic IDs in quick succession, so `peers_for_topic` climbed from
1 to 7 across them as earlier runs' registrations hadn't yet expired
under rlcore's 30s TTL — the relay/rlcore was also wasting some effort
routing toward those stale dead peers, so the true degradation curve
for raw is likely slightly better than shown. The direction of the
finding — and the compressed-only result, run on a fresh topic — both
hold regardless.)

---

## Step 13 — NAT traversal (cross-network nodes)

**Description:** How relink-rlcore --nat, hole punching, background re-punch, and the relay fallback work together.
**Tutorial Level:** Advanced

**◀ Previous:** [Step 12 — Benchmarks](#step-12--benchmarks) &nbsp;|&nbsp; **Next ▶:** [Step 14 — Troubleshooting](#step-14--troubleshooting)

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

**Background re-punch.** Mode A (rlcore, the only mode `--nat` applies
to) enables this automatically, at a 1s interval, with no call needed —
that's the mode where a peer's real endpoint was learned from a
NAT-mapped source port that can drift or expire, so continuously
rechecking readiness matters enough to not be opt-in. Instead of
punching a peer only once, right when it's first learned, a background
thread keeps re-punching every known peer on a timer for the node's
whole lifetime. Call `node.enable_nat_repunch(interval_seconds = 5.0)`
yourself (before `spin()`/`publish()` traffic) to pick your own interval
or to turn it on under Mode B/multicast too — an explicit call always
overrides the Mode A default. This fixes a real but narrow class of
failure: a marginal NAT whose
mapping expires faster than expected, or a peer discovered on one side
just before the other side's mapping timed out. It does **not** fix a
NAT/firewall that structurally drops all unsolicited inbound UDP
regardless of timing — verified against a real mobile-carrier NAT with a
raw (non-ReLink) hole-punching test: 85 retry packets over 22 seconds
still delivered zero packets in that direction, and a packet capture on
the receiving host's own network interface confirmed the inbound side's
NAT was dropping them before they ever arrived (not a local firewall —
`ufw` was inactive throughout). No amount of client-side retrying opens
a path that was never open; that case needs a relay/TURN-style fallback
— see below.

**Relay fallback, for NATs punching can't cross at all.** Run
`relink-relay` (C++, `cpp/rlcore/relink_relay.cpp`) or `relink_relay.py`
(pure Python, no compiler needed) on a host both nodes can reach — the
same box running `relink-rlcore --nat` works fine. Then call
`node.set_relay(ip, port = 8446)` on every node that needs it, before
`spin()`/`publish()` traffic. A relay reaches nodes direct punching
structurally cannot, because both clients only ever open a NAT mapping
toward the relay's one fixed `(ip, port)`, never toward each other — the
relay's replies always come from that exact remote endpoint, which is
exactly the case every stateful NAT/firewall allows back in (the same
mechanism that lets `relink-rlcore` itself register successfully from
behind a NAT).

This is a redundant *second* path, not a detect-failure-then-switch one:
once enabled, every `publish()` also goes to the relay, and every
topic's socket also registers with (and is kept alive at) the relay, the
whole time — direct hole punching keeps running exactly as before. A
subscriber that receives the same message from both paths silently
drops the second copy, matched by `seq_num` — a genuine duplicate can
never be mistaken for a new message, since a `seq_num` is assigned once
per `publish()` call and never reused. This trades a bit of constant
relay bandwidth for not needing an ack protocol to detect whether direct
delivery actually worked.

Performance: the relay never re-encodes a frame — it forwards the exact
bytes `recvfrom` already put in its buffer, peeking only the `topic_id`
that's already sitting in `RelinkHeader` at a fixed offset, so a
forwarded frame decodes on the receiving end identically to a direct one
(same `seq_num`, same payload). The C++ relay reuses one fixed buffer
for every forward — no heap allocation on the forwarding path at all.
`Image` never goes through the relay (like `set_multiplex(false)`, it
always stays off this path — a several-hundred-chunk burst mirrored
through a relay would be expensive for little benefit; a dropped Image
frame is already tolerated, see the troubleshooting table below).

## Step 14 — Troubleshooting

**Description:** Common symptoms, what they mean, and the fix, in one table.
**Tutorial Level:** Beginner

**◀ Previous:** [Step 13 — NAT traversal (cross-network nodes)](#step-13--nat-traversal-cross-network-nodes) &nbsp;|&nbsp; **Next ▶:** [Step 15 — Technical deep dive](#step-15--technical-deep-dive)

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

## Step 15 — Technical deep dive

**Description:** The harder internals behind earlier steps — kernel-level details, exact measured numbers, and design tradeoffs — kept separate from the plain-language walkthrough above.
**Tutorial Level:** Advanced

**◀ Previous:** [Step 14 — Troubleshooting](#step-14--troubleshooting)

Nothing here is required to use ReLink. Every earlier step works fully
without reading this one — this is for when you want to know exactly
*why*, not just *how*.

### Why ReLink gives up ROS's generality

ROS1's TCPROS pays a per-message connection-management tax and depends
on a single master. ROS2's DDS fixes the master but replaces it with
continuous multicast re-announcement (SPDP) that scales as O(N²) with
node count and grows the longer a system stays up. Neither was designed
around a hard real-time latency floor — both were designed around
generality (arbitrary QoS, arbitrary transports, arbitrary
serialization), and generality has a cost. ReLink gives up that
generality — one message type per topic, no reliable-transport path in
v1, no schema evolution — in exchange for a fixed-layout message over
raw UDP, discovered once (not re-announced forever), serialized by a
straight `memcpy`, dispatched on a dedicated thread with no lock in the
hot path. See [Step 0](#step-0--what-relink-actually-is) for the
plain-language summary and [Step 12](#step-12--benchmarks) for the
measured numbers this buys.

### `network_id` — why both the address and the port have to change

Mode B's multicast domain isolation ([Step 3](#step-3--pick-a-discovery-mode))
makes nodes with different `network_id` values join different
multicast group addresses *and* different ports. Both have to vary
together: an earlier design that only shifted the address (mirroring
the address-shifting half of how ROS assigns domains) turned out to
leak across domains on Linux specifically, because
`MulticastDiscovery`'s listener socket sets `SO_REUSEPORT` (needed so
several ReLink nodes can share one host on the same multicast port) —
and `SO_REUSEPORT`'s delivery selection is scoped by port only, so two
sockets bound to the *same port* but joined to *different* multicast
addresses still both received a beacon meant for only one of them, a
directly-reproduced kernel behavior on this project's own test machine.
Varying the port too sidesteps it entirely. No beacon wire-format
change — isolation is entirely about which address/port a node's
socket joins, not anything inside the packet. Verified: same
`network_id` on both sides (including cross-language, C++ publisher to
Python subscriber) discovers and delivers correctly; different values
produce zero cross-talk in either direction.

### Named-topic resolution cost (Step 4)

Every call to the string overload (`publish<T>(name, ...)`,
`advertise<T>(name, ...)`, `subscribe<T>(name, ...)`) re-hashes the
string (FNV-1a over every character), takes the node's internal lock,
and does a registry lookup+comparison — every single call, not just the
first. Measured effect (microbenchmark, no peers attached, isolating
just the resolution cost): the numeric overload costs **~26 ns/call**,
the string overload **~49 ns/call** — the FNV-1a hash + lock + registry
lookup roughly **doubles** per-call overhead versus a bare numeric id.
At the multi-hundred-kHz burst rates in [Step 12](#step-12--benchmarks),
that difference is exactly the kind of per-message tax that determines
whether the sender or receiver becomes the bottleneck first — resolve
once with `topic_id_for()`/`_topic_id_for()`, publish by id.

### AF_XDP fast path, under the hood

`cpp/rlcore/relink_relay.cpp`, built with `-DRELINK_ENABLE_XDP`, has
the relay bypass the Linux kernel's normal UDP receive path for matched
traffic via a native/driver-mode XDP program + AF_XDP socket (falls
back to a plain socket automatically if the kernel/driver/toolchain
don't support it — see `cpp/rlcore/xdp/relay_xdp.hpp`). It needs
`libbpf`, `clang`, and Linux ≥ 5.1 to build, and root/`CAP_NET_ADMIN`
to run — none of which are needed for the plain-socket relay ([Step
9](#step-9--running-the-rlcore-daemon)). Missing build dependencies
fail `cmake`'s configure step with the exact `apt-get install` line
needed, rather than a confusing compile error. Measured numbers for
this path are in [Step 12's AF_XDP section](#af_xdp-fast-path).

### Porting the C++ core to Windows

`relink/platform.hpp` is the one file that knows the difference between
POSIX sockets and Winsock; every other header/source file is written
once and compiles unchanged on both, via a handful of portable
wrappers (`close_socket()`, `set_recv_timeout_ms()`, `sendmsg_to()`,
`pin_thread_to_core()`, `set_thread_realtime()`). Three real platform
gaps had to be worked around, not just renamed:

- **A Windows header name collision.** `windows.h`'s GDI header
  declares a global `Polygon()` function, which collided with
  ReLink's own `geometry_msgs::Polygon` (Step 7) once `using namespace
  geometry_msgs;` was in scope — a genuine ambiguous-symbol compile
  error, not a typo. Fixed with `#define NOGDI` before including
  `windows.h`, since ReLink never touches GDI anyway.
- **`SO_RCVTIMEO` has a different wire shape.** POSIX takes a `struct
  timeval` (seconds + microseconds); Winsock takes a single `DWORD` of
  milliseconds. `set_recv_timeout_ms()` hides that difference behind
  one signature both platforms implement.
- **No `sendmsg()`/scatter-gather send on Windows** without
  `WSASendMsg` (which needs a runtime function-pointer lookup via
  `WSAIoctl`, not a plain link-time symbol). Windows' `sendmsg_to()`
  instead copies every `iovec` segment into one ≤1500-byte stack
  buffer and sends it with a single `sendto()` — not zero-copy like
  the POSIX path (see `udp_transport.hpp::publish_scattered`'s use for
  `Image`, Step 8), but correct, and payloads here are always well
  under that size.

AF_XDP (this section, above) stays Linux-only regardless of the
`RELINK_ENABLE_XDP` build flag — it's gated out under `_WIN32`, since
the whole mechanism (eBPF, UMEM, native XDP sockets) doesn't exist on
Windows; the plain-socket relay is the only path there.

**Verification method**: rather than trusting that a clean
cross-compile means a working port, the Windows build was actually
*run*, under Wine (`x86_64-w64-mingw32-g++` to build, a 64-bit Wine
prefix to execute) — all 10 C++ test binaries passing, a native Linux
`hello_relink` and a Windows `.exe` (under Wine) exchanging messages
in both directions over real multicast, a Linux client registering
against a Windows-hosted `relink-rlcore` daemon, a sustained 1000Hz/60s
throughput run (Windows publisher, Linux subscriber, effectively zero
loss in steady state), and a 6-node concurrent stress test (3 Windows
+ 3 Linux processes, 30s, zero crashes across ~90,000 total logged
messages). macOS shares the POSIX branch of `platform.hpp` (real
sockets, real `sendmsg()`) and is believed correct by the same
reasoning, but hasn't been run on real hardware the way the Windows
port was verified under Wine.

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
| Relay fallback (`set_relay`) | ✅ | ✅ |
| `Image` type (chunk + reassemble) | ✅ | ✅ |
| Zero-copy image chunk send/recv | ✅ (`sendmsg`) | ✅ (`socket.sendmsg`) |
| CPU pinning / `SCHED_FIFO` data thread | ✅ | — (not applicable to CPython's threading model) |
| TCP reliable transport | ❌ (out of scope) | ❌ (out of scope) |
| Encryption (`secure=true`) | ❌ (API shape decided, unimplemented) | ❌ (API shape decided, unimplemented) |

### Repository layout

Everything C++ lives under `cpp/`, everything Python lives under
`python/` — same two-language split as the rest of this doc, so it's
never ambiguous which build tooling a given file needs.

```
cpp/
  CMakeLists.txt                Top-level CMake project -- builds everything below in one shot
  include/relink/               C++ library (header-only)
    wire.hpp                     byte-exact structs: RelinkHeader, BeaconPacket, default types
    topic_hash.hpp                FNV-1a 32-bit hash for named topics
    ring_buffer.hpp               fixed-capacity, drop-oldest-on-overflow
    frame.hpp                     pure encode/decode, MTU-budgeted
    udp_transport.hpp             dedicated data thread, CPU pinning, SCHED_FIFO
    register.hpp / rlcore_client.hpp       mode A (rlcore) client
    beacon.hpp / multicast_discovery.hpp    mode B (multicast) client
    relink.hpp                    RelinkNode -- the public API
    relay_wire.hpp                 relay fallback wire helpers (Step 13)
    crypto.hpp                     AES-256-GCM for the rlcore signaling handshake

  rlcore/                       C++ daemons (Step 9)
    relink_rlcore.cpp             registration daemon
    relink_relay.cpp              relay fallback daemon (Step 13)
    CMakeLists.txt                 also buildable standalone; -DRELINK_ENABLE_XDP=ON for AF_XDP (Step 12)
    xdp/                           AF_XDP socket + eBPF kernel program

  tools/                        rl_topic.cpp, relink_example.cpp, relink_benchmark.cpp
  examples/                     C++ usage examples (Step 10)
  tests/                        unit tests + two-process correctness tests (Step 11)
  cmake_example/                minimal standalone CMake template for consuming ReLink from your own project
  ros2_compare/                  ROS2 Humble comparison benchmark package

python/
  relink_py/relink/             Python library (stdlib-only: ctypes + socket + struct)
    (mirrors the C++ layer-for-layer, see python/relink_py/README.md)
  relink_py/examples/            Python usage examples (Step 10)
  relink_py/tests/               unit tests + two-process correctness tests (Step 11)
  rlcore/                        Python daemons, byte-identical wire protocol to cpp/rlcore/
    relink_rlcore.py               registration daemon
    relink_relay.py                relay fallback daemon (Step 13)
  rl_topic.py
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

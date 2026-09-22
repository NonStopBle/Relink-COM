# ReLink pub/sub quickstart (`cpp/src/`)

Four minimal files showing the ways to structure a ReLink node — pick
whichever matches your project's shape:

| File | What it is | Run it with |
|---|---|---|
| `pub.cpp` | Publisher only | a copy of `sub.cpp` (or `pubsub.cpp`/`main.cpp`) on the other end |
| `sub.cpp` | Subscriber only | a copy of `pub.cpp` (or `pubsub.cpp`/`main.cpp`) on the other end |
| `pubsub.cpp` | Both roles in one node | another copy of itself, `main.cpp`, or `pub.cpp`/`sub.cpp` |
| `main.cpp` | Same as `pubsub.cpp`, named `main.cpp` for build tooling that expects that entry-point filename | same as `pubsub.cpp` |

All four talk on the same topic (`/example/chatter`) with the same
message shape, so any combination of them can talk to each other. This
file covers only "how do I build and run these" — for the full API
(message types, images, NAT traversal, etc.) see the
[main project README](../../README.md).

**Fastest path** (from the repo root):

```bash
cd cpp && cmake -B build . && cmake --build build
./build/src/sub      # second terminal: ./build/src/pub
```

## Building

These are already wired into the top-level `cpp/CMakeLists.txt`:

```bash
cd cpp
cmake -B build .
cmake --build build
./build/src/pub      # or: ./build/src/sub, ./build/src/pubsub, ./build/src/main
```

Or use [`../build.sh`](../build.sh), a thin wrapper around the same
commands: `./build.sh`, `./build.sh --windows` (cross-compile), or
`./build.sh --test` to also run the unit tests.

Or compile a single file directly with `g++`/`clang++`, the same way
as any other example in this repo:

```bash
g++ -std=c++17 -I cpp/include -pthread cpp/src/pub.cpp -o pub
g++ -std=c++17 -I cpp/include -pthread cpp/src/sub.cpp -o sub
```

On Windows, add `-lws2_32` instead of `-pthread` (see the main
README's "Building the C++ library, per OS" in Step 1, and
[`cpp/cmake_example/`](../cmake_example/) for a copy-pasteable CMake
template if you're starting a new project rather than building inside
this repo).

## Running

In two terminals (or on two machines on the same LAN):

```bash
./sub
./pub
```

`sub` should start printing `received: hello 0`, `received: hello 1`,
... shortly after `pub` starts. If you see `(no peers found yet...)`
on the publisher side for more than a few seconds, that's usually just
discovery timing (ReLink's multicast beacon has a startup burst, then
a sparse ~30-60s reannounce interval — see the main README's
Troubleshooting step), not a bug; it resolves itself.

## Using this as a starting point for your own project

Copy whichever file(s) match your node's shape into your own project
alongside ReLink's headers (`cpp/include/relink/`), rename the topic
string and message struct, and build the same way. See
[`cpp/cmake_example/`](../cmake_example/README.md) for a full
copy-pasteable CMake project template if you're starting a brand new
project rather than adding a file to this repo.

## Building `relink-rlcore` and `rl_topic`

`pub.cpp`/`sub.cpp`/`pubsub.cpp` all use
`node.use_multicast_discovery()`, which needs no daemon — fine for
nodes on the same LAN. If your nodes are spread across networks, swap
that one line for `node.set_rlcore.ip("...")` and point it at a
running `relink-rlcore` instance instead.

**Building `relink-rlcore`** (the registration daemon):

```bash
cd cpp/rlcore
cmake -B build .
cmake --build build
./build/relink-rlcore --port 8445
```

(Or, via the top-level project: `cd cpp && cmake -B build . &&
cmake --build build && ./build/rlcore/relink-rlcore --port 8445` --
same daemon, built alongside everything else in one pass.)

Useful flags (full details in the main README's Step 9):

```bash
./relink-rlcore --nat                     # NAT traversal / UDP hole punching
./relink-rlcore --generate-key            # print a fresh AES-256 key
./relink-rlcore --encrypt-key <64-hex>    # require encrypted registration
```

**Building `rl_topic`** (a `rostopic`-style inspection CLI — only
buildable via the top-level project, since it isn't part of
`rlcore/`'s own standalone `CMakeLists.txt`):

```bash
cd cpp
cmake -B build .
cmake --build build
./build/rl_topic list --rlcore-ip 203.0.113.10
./build/rl_topic info /example/chatter --rlcore-ip 203.0.113.10
./build/rl_topic echo /example/chatter --rlcore-ip 203.0.113.10
```

`hz`/`bw`/`echo`/`pub` also take an `--ipc` flag that targets a
same-host shared-memory topic directly instead of the network — no
`relink-rlcore`/multicast involved at all:

```bash
./build/rl_topic echo /my/ipc/topic --ipc
./build/rl_topic pub /my/ipc/topic --ipc --text "hello over shm"
```

Prebuilt Windows binaries for both are also available under
[`cpp/win-bin/`](../win-bin/) if you don't want to build them yourself.

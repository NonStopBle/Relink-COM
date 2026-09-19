# ReLink CMake project template

A minimal, self-contained CMake project showing how to consume
ReLink's header-only C++ core from **your own** project (rather than
building inside the ReLink repo itself). `main.cpp` in this directory
is a working pub/sub node you can run right now, in two terminals, to
see it work end to end before you change anything.

For the full API walkthrough, discovery modes, message types, NAT
traversal, and everything else, see the
[main project README](../../README.md) — this file only covers
"how do I use this template."

## What's in this directory

```
cmake_example/
  CMakeLists.txt              builds main.cpp against ../include/relink/
  main.cpp                    a working pub/sub node (multicast discovery)
  mingw-w64-toolchain.cmake   cross-compile for Windows from Linux/WSL
  README.md                   this file
```

## Quick start (this repo, as-is)

```bash
cd cpp/cmake_example
cmake -B build .
cmake --build build
./build/relink_app          # run this in a SECOND terminal too
```

Run it in two terminals (or on two machines on the same LAN) and each
copy discovers the other over multicast and prints what it receives —
no daemon, no config file, nothing else to start first.

## Using this as a template for your own project

1. **Copy this whole directory** into your own project, e.g.:

   ```bash
   cp -r cpp/cmake_example /path/to/your_project/relink_app
   ```

2. **Copy or vendor ReLink's headers** somewhere your project can see
   them — the simplest option is copying `cpp/include/relink/` itself
   (it's header-only, nothing to build):

   ```bash
   cp -r cpp/include /path/to/your_project/third_party/relink_include
   ```

3. **Edit `CMakeLists.txt`**: change `RELINK_INCLUDE_DIR` to wherever
   you put the headers in step 2, and change `main.cpp` in the
   `add_executable(relink_app main.cpp)` line to your own source
   file(s):

   ```cmake
   set(RELINK_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/relink_include)
   add_executable(relink_app my_node.cpp)   # or your own file name / multiple files
   ```

   Nothing else needs to change — the `WIN32`/`else()` branch already
   handles `ws2_32` vs. `pthread` for you on every OS.

4. **Build and run** the same way as the quick start above.

## Building for Windows

Native (MSYS2/MinGW-w64 shell, or Visual Studio's own CMake support):

```bash
cmake -B build .
cmake --build build
build\relink_app.exe
```

Cross-compiled from Linux/WSL (what this project's own CI/verification
uses — see the main README's Step 15 for the Wine-based verification
this was actually tested with):

```bash
sudo apt-get install -y g++-mingw-w64-x86-64-posix   # once
cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=mingw-w64-toolchain.cmake .
cmake --build build-win
wine build-win/relink_app.exe   # or copy the .exe to a real Windows machine
```

`mingw-w64-toolchain.cmake` is generic — copy it along with
`CMakeLists.txt` into your own project, no edits needed.

## Switching from multicast to `rlcore` discovery

`main.cpp` uses `node.use_multicast_discovery()` — zero setup, but only
works when every node is on the same LAN segment (multicast doesn't
cross most routers). If your nodes are spread across subnets, or you
just want a single well-known rendezvous point, switch to Mode A:

```cpp
// Instead of node.use_multicast_discovery():
node.set_rlcore.ip("203.0.113.10");   // wherever relink-rlcore is running
```

That's the only line that changes in `main.cpp` — `advertise()`,
`subscribe()`, and `publish()` all stay exactly the same regardless of
discovery mode. Mode A needs `relink-rlcore` running somewhere both
nodes can reach; see the next section for how to build it.

## Building `relink-rlcore` and `rl_topic`

These live in the main ReLink repo (`cpp/rlcore/` and `cpp/tools/`),
not in this template directory — you build them once, run
`relink-rlcore` as a long-lived daemon, and your own project's nodes
(built from this template) just point at its address.

**`relink-rlcore`** — the registration daemon Mode A nodes register
with:

```bash
cd cpp/rlcore
cmake -B build .
cmake --build build
./build/relink-rlcore --port 8445
```

Or build it directly with the top-level project (`cpp/CMakeLists.txt`
builds this and everything else in one shot):

```bash
cd cpp
cmake -B build .
cmake --build build
./build/rlcore/relink-rlcore --port 8445
```

Useful flags (full details in the main README's Step 9):

```bash
./relink-rlcore --nat                     # NAT traversal / UDP hole punching
./relink-rlcore --generate-key            # print a fresh AES-256 key for the next flag
./relink-rlcore --encrypt-key <64-hex>    # require encrypted registration (see main README)
```

**`rl_topic`** — a `rostopic`-style CLI for inspecting what's
registered with a running `relink-rlcore` (or reachable over
multicast), useful while developing against this template:

```bash
cd cpp   # top-level project
cmake -B build .
cmake --build build
./build/rl_topic list --rlcore-ip 203.0.113.10
./build/rl_topic info /example/chatter --rlcore-ip 203.0.113.10
```

Both binaries are also available prebuilt for Windows under
[`cpp/win-bin/`](../win-bin/) if you don't want to build them yourself.

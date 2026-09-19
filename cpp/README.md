# ReLink — C++ core

This directory is the C++ implementation of ReLink: a header-only
pub/sub library (`include/relink/`), a small registration daemon
(`rlcore/`), a topic-inspection CLI (`tools/rl_topic.cpp`), and
examples/tests for all of it.

For the full walkthrough — discovery modes, message types, images, NAT
traversal, benchmarks, troubleshooting — see the
[main project README](../README.md). This file only covers what's in
`cpp/` and how to build it.

## Layout

```
cpp/
  CMakeLists.txt        top-level project -- builds everything below in one shot
  build.sh               wrapper script around the cmake commands (--windows, --test, ...)

  include/relink/        the library itself (header-only, nothing to link)
    relink.hpp             RelinkNode -- the public API
    wire.hpp                byte-exact wire structs
    udp_transport.hpp       data-thread transport, CPU pinning, SCHED_FIFO
    register.hpp / rlcore_client.hpp     Mode A (rlcore) discovery
    beacon.hpp / multicast_discovery.hpp Mode B (multicast) discovery
    crypto.hpp               AES-256-GCM for the rlcore signaling handshake
    platform.hpp             Linux/macOS/Windows socket + threading shim
    ... (topic_hash.hpp, ring_buffer.hpp, frame.hpp, image.hpp, standard_msgs.hpp, relay_wire.hpp, topic_directory.hpp)

  rlcore/                registration daemon + relay (own standalone CMakeLists.txt)
    relink_rlcore.cpp       the daemon nodes register with (Mode A)
    relink_relay.cpp        NAT-fallback relay
    xdp/                    optional AF_XDP fast path (Linux only)

  src/                   minimal pub/sub quickstart -- see src/README.md
    pub.cpp, sub.cpp, pubsub.cpp, main.cpp

  tools/                 rl_topic.cpp (rostopic-style CLI), relink_example.cpp, relink_benchmark.cpp
  examples/              larger, message-type-coverage examples (Step 10 of the main README)
  tests/                 unit tests + two-process correctness tests
  cmake_example/         standalone CMake project template for consuming ReLink from your OWN project -- see cmake_example/README.md
  win-bin/               prebuilt Windows .exe binaries (relink-rlcore, rl_topic)
  ros2_compare/           ROS 2 Humble comparison benchmark package (unrelated to using ReLink itself)
```

## Building

The whole tree, in one shot:

```bash
cd cpp
cmake -B build .
cmake --build build
ctest --test-dir build          # runs the 10 self-contained unit tests
```

Or with the wrapper script:

```bash
./build.sh                # same as above
./build.sh --test         # build + run ctest
./build.sh --windows      # cross-compile for Windows (needs g++-mingw-w64-x86-64-posix)
./build.sh --clean        # wipe build/ and build-win/ first
./build.sh --help
```

`-DRELINK_BUILD_EXAMPLES=OFF`/`-DRELINK_BUILD_TESTS=OFF` (or
`./build.sh --no-examples`/`--no-tests`) skip those directories if you
only want the daemon/relay/tools. `camera_stream` only builds when
`find_package(OpenCV)` succeeds — everything else needs no extra
dependencies. Verified on native Linux (full build + all tests) and
cross-compiled for Windows via MinGW-w64, actually run under Wine (see
the main README's Step 15 for details).

Since the library itself is header-only, you don't need any of the
above just to *use* ReLink in your own code — compiling a single file
against `include/relink/` directly works too:

```bash
g++ -std=c++17 -I cpp/include -pthread your_node.cpp -o your_node
```

Starting a brand new project instead of adding a file to this repo?
Use [`cmake_example/`](cmake_example/) as a copy-pasteable template —
it's self-contained (its own `CMakeLists.txt`, `main.cpp`, and a MinGW
cross-compile toolchain file) and documents adapting it step by step.

## Where to start reading

- **Just want to see a working node?** [`src/pub.cpp`](src/pub.cpp),
  [`src/sub.cpp`](src/sub.cpp), [`src/pubsub.cpp`](src/pubsub.cpp) —
  each is under 60 lines. [`src/README.md`](src/README.md) covers
  building/running them and starting your own project from them.
- **Building against ReLink from an external project?**
  [`cmake_example/`](cmake_example/README.md).
- **Running the registration daemon or inspecting topics?**
  `rlcore/relink_rlcore.cpp` and `tools/rl_topic.cpp` — build steps and
  every CLI flag (including `--nat` and the AES-256-GCM
  `--encrypt-key`/`--generate-key` encryption flags) are in the main
  README's Step 9.
- **Want to see every built-in/composite message type in action?**
  [`examples/`](examples/) — one file per ROS-familiar message package.
- **Modifying the library itself?** Start at
  [`include/relink/relink.hpp`](include/relink/relink.hpp) (the public
  API) and follow its includes down into `udp_transport.hpp`,
  `register.hpp`/`beacon.hpp`, etc. Run `tests/` (via `ctest`) after
  any change.

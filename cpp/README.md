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
    shm_transport.hpp        same-host shared-memory IPC ring (advertise/subscribe/publish_local_ipc)
    ... (topic_hash.hpp, ring_buffer.hpp, frame.hpp, image.hpp, compressed_image.hpp,
         adaptive_bitrate.hpp, standard_msgs.hpp, relay_wire.hpp, topic_directory.hpp)

  rlcore/                registration daemon + relay (own standalone CMakeLists.txt)
    relink_rlcore.cpp       the daemon nodes register with (Mode A)
    relink_relay.cpp        NAT-fallback relay
    xdp/                    optional AF_XDP fast path (Linux only)

  src/                   minimal pub/sub quickstart -- see src/README.md
    pub.cpp, sub.cpp, pubsub.cpp, main.cpp

  tools/                 rl_topic.cpp (rostopic-style CLI), relink_example.cpp, relink_benchmark.cpp,
                          relink_image_benchmark.cpp (raw image UDP-vs-shm-IPC benchmark, needs OpenCV)
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
ctest --test-dir build          # runs the 12 self-contained unit tests
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

## Copying ReLink into your own project

ReLink's C++ core is header-only — there is no library to build or
`.so`/`.a` to link, and no package manager integration needed. "Using
it in your project" means copying one directory and pointing your
compiler's include path at it.

1. **Copy the library.** Only `include/relink/` is needed — nothing
   else in this repo (`rlcore/`, `examples/`, `tests/`, etc.) is
   required at all to use ReLink as a library:

   ```bash
   cp -r cpp/include/relink /path/to/your_project/third_party/relink
   ```

   This is a real copy, not a reference back into this repo — your
   project owns it from here and can pin it at whatever commit you
   copied it from. There's no build step to re-run when you update it,
   just copy the newer `include/relink/` over the old one.

2. **Point your build at it.** Whatever build system you use, add
   `third_party/` (the directory *containing* `relink/`, not `relink/`
   itself) to your include path, since every header includes its
   siblings as `#include "relink/something.hpp"`:

   - **Raw compiler command:**
     ```bash
     g++ -std=c++17 -I third_party -pthread my_node.cpp -o my_node
     ```
   - **CMake** (see [`cmake_example/CMakeLists.txt`](cmake_example/CMakeLists.txt)
     for the full working file, including the Windows/`ws2_32` and
     macOS/Linux `pthread` handling this snippet omits for brevity):
     ```cmake
     target_include_directories(my_target PRIVATE third_party)
     ```
   - **Any other build system** (Makefile, Bazel, Meson, ...): add
     `third_party` as a header search path the same way you would for
     any other vendored header-only library.

3. **`#include "relink/relink.hpp"`** in your code and use
   `RelinkNode` — see [`src/pub.cpp`](src/pub.cpp)/[`src/sub.cpp`](src/sub.cpp)
   for the smallest possible working example, or the main README for
   the full API.

4. **Windows** needs `-lws2_32` (or `target_link_libraries(... ws2_32)`
   in CMake) in addition to the include path — nothing else changes,
   the same headers work unmodified on Linux, macOS, and Windows (see
   the main README's Step 15 for what `platform.hpp` handles
   internally so you don't have to).

That's the whole integration. If you'd rather start from a working
project than wire this up by hand, copy
[`cmake_example/`](cmake_example/) instead of doing steps 2-3
yourself — it's already set up exactly as described above, just with
`../include` pointing back into this repo instead of a copied
`third_party/relink/`; change that one path and it's identical to what
step 2 produces.

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
- **Publisher and subscriber on the same machine?**
  `advertise_local_ipc`/`subscribe_local_ipc`/`publish_local_ipc` (and
  the `_image` variants) route through `shm_transport.hpp`'s
  shared-memory ring instead of UDP — see the main README's [same-host
  shared-memory IPC](../README.md#same-host-shared-memory-ipc-_local_ipc)
  section for the full API and constraints. Want to measure the
  difference yourself? `tools/relink_image_benchmark.cpp` (needs
  OpenCV) runs raw full-HD frames both ways and reports delivery rate
  and throughput for each — see the main README's Step 12 for the
  measured numbers.
- **Modifying the library itself?** Start at
  [`include/relink/relink.hpp`](include/relink/relink.hpp) (the public
  API) and follow its includes down into `udp_transport.hpp`,
  `register.hpp`/`beacon.hpp`, etc. Run `tests/` (via `ctest`) after
  any change.

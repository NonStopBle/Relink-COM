# ReLink -- Task List (personal notes, not part of the public repo)

Everything implemented together so far, in order.

## Core protocol / v1

- [x] Initial implementation of ReLink-COM v1 (steps 1-11: wire format, UDP
      transport, registration/discovery, core pub/sub API)
- [x] Python bindings + multicast discovery examples for both languages
- [x] UDP NAT traversal support in com-core (`--nat` flag)
- [x] Root README with project abstract and usage guide
- [x] Project abstract added to relink_py README
- [x] Real cross-machine NAT hole-punching test program
- [x] Rewrote README for external users; added hello_relink and
      camera_stream examples

## Image transport

- [x] Built-in Image type with automatic MTU chunking (C++ and Python)
- [x] Sized UDP socket buffers for multi-chunk Image bursts (4MB default)
- [x] Documented the buffer-size/latency tradeoff for subscribe_image
- [x] Lowered default socket buffer from 4MB to 1MB (measured minimum)
- [x] Scoped the large socket buffer to Image only; added zero-copy chunk I/O
- [x] Fixed bogus "effective sustained rate" metric in both benchmarks
- [x] Removed misleading "sustained rate" numbers from Benchmarks table

## topic_id widen (uint16_t -> uint32_t), rlcore rename, named topics

- [x] Widened topic_id to uint32_t, renamed comcore to rlcore, added named
      topics
- [x] Widened two_process test CLI topic_id parsing to uint32_t
- [x] Added network-wide `rltopic list`, `rl_topic` CLI, full standard_msgs
      catalog, per-topic-port mode

## NAT / relay hardening

- [x] Documented multiplex vs per-topic-port benchmark results
- [x] Fixed NAT punch routing for `set_multiplex(false)`, added background
      re-punch
- [x] Added relay fallback for NATs that direct hole punching cannot cross
- [x] Auto-enabled 1s NAT repunch thread in rlcore mode by default
- [x] Fixed relay dedup race and false-negative window
- [x] Added opt-in AF_XDP fast path for relink-relay
- [x] Made rlcore a CMake project with `RELINK_ENABLE_XDP` build flag
- [x] Fixed AF_XDP relay silently dropping non-redirected traffic

## Repo structure / docs pass 1

- [x] Split repo into cpp/ and python/, expanded README with real test
      results
- [x] Added topic role tracking, custom message schemas, camera streaming
      comparisons
- [x] Added topic pairing (`set_multiplex(false)`) and network_id domain
      isolation (Mode B)
- [x] Fixed pair-then-unpair to throw; restyled README like ROS wiki
      tutorials
- [x] Restructured quick-start steps to follow ROS wiki tutorial format
- [x] Added "Review recap" sections to tutorial-style README steps
- [x] Added custom_types_pubsub example (C++ + Python)
- [x] Added builtin_types_pubsub example, listed all built-in message types
- [x] Added standard_msgs_pubsub example covering all 46 composite types
- [x] Added one example per message package, plus std_msgs MultiArray family
- [x] Linked message-type examples directly from Step 7 in README
- [x] Widened two_process test CLI topic_id parsing to uint32_t (dup fix)
- [x] Simplified wording in README steps, moved deep internals to a new
      "Technical deep dive" step
- [x] Added beginner glossary, simplified jargon-heavy wording in early
      README steps
- [x] Retargeted glossary/wording for network/SWE/mechatronics background
      instead of zero-knowledge readers

## Cross-platform

- [x] Added Windows support to the C++ core, verified with MinGW + Wine
- [x] Documented per-OS C++ build steps; fixed missing `<cstring>` include;
      added macOS thread-pinning fallback
- [x] Added prebuilt Windows binaries for relink-rlcore and rl_topic

## Encryption

- [x] Added AES-256-GCM encryption for the rlcore registration handshake
- [x] Documented why payload encryption is out of scope (performance)
- [x] Expanded rlcore encryption docs into a full setup/troubleshooting
      manual
- [x] Added Python support for rlcore encryption; added a top-level cpp/
      CMake project

## cpp/ as a conventional CMake project

- [x] Added a copy-pasteable CMake example project for the C++ core
- [x] Restructured cpp/ to look like a conventional CMake project
- [x] Added cpp/src/ pub.cpp, sub.cpp, pubsub.cpp quickstart + README
- [x] Added cpp/src/main.cpp as the conventional entry-point filename
- [x] Added a working pub/sub main.cpp and README to the CMake template
- [x] Added cpp/build.sh, a wrapper script around the cmake build commands
- [x] Added cpp/README.md documenting the C++ directory layout and build
      steps
- [x] Documented copying ReLink's header-only library into an external
      project (both cpp/README.md and the main README)
- [x] Stopped tracking relink_com_implement.md

## Docs pass 2 / pip packaging / this session

- [x] Renamed project title to ReLink-COM in README
- [x] Reframed README tagline around ReLink's own problem, not a ROS
      comparison
- [x] Rewrote README intro paragraph in more technical/networking terms
- [x] Added a Features section to the README, left-aligned/formal style
- [x] Made python/ pip-installable: relink_py package restructured with a
      cli/ subpackage, console_scripts entries for `rlcore`, `rltopic`,
      `relink-relay`, plus python/README.md
- [x] Added `--ip <address>` and fixed `-h`/`--help` (was falling through
      to a bind attempt) on relink-rlcore, both C++ and Python builds;
      rejected unrecognized flags; clearer bind-failure messages
- [x] Ran a fresh concurrent stress test (3 native + 3 Wine) -- clean, zero
      errors
- [x] Gitignored cpp/build/, cpp/build-win/, *.egg-info/
- [x] Created this docs/ folder (gitignored, personal notes) and moved
      relink_com_implement.md, relink-com-spec.md, COMPARISON.md into it

## In progress / pending

(none right now -- see "Verified done" below for the item that was here)

## Verified done (2026-09-20 check)

- [x] Widen `topic_id` from `uint16_t` to `uint32_t` -- confirmed this was
      already fully merged (wire.hpp/register.hpp/beacon.hpp/frame.hpp/
      udp_transport.hpp/multicast_discovery.hpp/relink.hpp all use
      `uint32_t topic_id`; RelinkHeader is 9 bytes; relink_py's wire.py
      uses `ctypes.c_uint32` and is also 9 bytes). image.hpp has no
      topic_id-typed fields, so nothing to fix there.
      Re-verified end to end:
      - full cpp/ rebuild via CMake -- clean
      - `ctest` -- 10/10 pass
      - `python3 tests/test_relink.py` and `tests/test_image.py` -- ALL PASS
      - manual cross-language byte check: encoded topic_id=0x12345678
        (>0xFFFF, only fits in 4 bytes) in Python's `encode_frame`, header
        bytes matched the same LE layout C++'s test_wire.cpp expects
        (topic_id, seq_num, payload_len, flags in that order)
      - fixed two stale comments left over from the old 7-byte header:
        cpp/include/relink/frame.hpp ("header(7)" -> "header(9)") and
        cpp/include/relink/udp_transport.hpp ("9 fixed framing bytes ...
        7-byte header" -> "11 fixed framing bytes ... 9-byte header")

## Ideas / not started

- [ ] Consider whether `--ip`/unrecognized-flag handling should also be
      added to `relink-relay` and `rl_topic` (only `relink-rlcore` got it
      so far, prompted by the readyidc bind-error report)
- [ ] Give relay.py's CLI `-h`/`--help` and `--ip` for parity with rlcore

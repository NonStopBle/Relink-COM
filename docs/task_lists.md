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

## Verified done (2026-09-20, continued)

- [x] `relay.py` (`relink-relay`): added `--port`, `--ip`, `-h`/`--help`,
      unrecognized-flag rejection, and a clearer bind-failure message,
      matching `rlcore.py`. Kept the legacy `relink-relay [port]`
      positional form working (errors if both `--port` and a positional
      are given, or if more than one positional is given).
- [x] `rl_topic.py` (`rltopic`): `-h`/`--help` was already free via
      argparse on the top-level command and every subcommand. Added
      `--ip` to `list`/`info` (and everything sharing the `common`
      argparse group) to bind the query socket to a specific local
      interface, and to set `IP_MULTICAST_IF` for multicast queries on a
      multi-homed host. Not wired into `hz`/`bw`/`echo`/`pub`, which go
      through `RelinkNode` instead of a raw socket here -- `RelinkNode`
      itself has no bind-ip option yet, so plumbing it through those four
      would be a separate, bigger change.
      Verified: `--help` on both tools and every rl_topic subcommand,
      `--ip 127.0.0.1` functional test, bad-flag rejection, bind-conflict
      message, both-port-forms-given error, both-positionals error, and
      the full `test_relink.py`/`test_image.py` suites (ALL PASS).
      README's relay/rl_topic sections updated to document the new flags.

## Verified done (2026-09-20, continued further)

- [x] Merged the standalone Python `relink-relay` daemon (`relay.py`)
      directly into `rlcore.py` -- one process, one port, instead of two
      daemons. New `--relay` flag turns on data-frame forwarding on
      rlcore's own socket (reusing `relay_wire.decode_relay_register`/
      `peek_frame_topic_id`, same forwarding logic relay.py had); `--nat`
      auto-enables `--relay` too, since a symmetric-NAT client (the case
      `--nat` mode exists for) is exactly the case that needs the relay
      fallback. Startup message reports which of NAT/relay are active
      and whether relay came from `--nat` or was requested directly.
      Decision (asked and confirmed by the user): remove the standalone
      `relay.py`/`relink-relay` console_script entirely rather than keep
      it alongside the merged form -- the C++ build still ships
      `relink-relay` as its own binary (unchanged; this was a Python-only
      merge), so parity between the two languages is now "same
      capability, different process topology" rather than "identical
      topology". Client wiring (`node.set_relay(ip, port)`) stays a
      manual call, same as before, just pointed at rlcore's own
      `--port` (8445 by default) instead of the old relay-only default
      of 8446 -- no RelinkNode API change.
      Verified: `rlcore --help` shows `--relay`; `--relay` alone and
      `--nat` (which implies it) both start correctly with the right
      startup message; a raw relay REGISTER + data-frame test forwarded
      correctly between two sockets; a full RelinkNode pub/sub pair
      using `set_relay()` pointed at the merged rlcore delivered a
      message end-to-end through the relay path; full
      `test_relink.py`/`test_image.py` suites still ALL PASS.
      README (root + python/README.md) updated throughout to describe
      the merged Python daemon vs. the still-separate C++ binary, and
      the stale python/ directory-layout diagram (still showing the
      pre-pip-packaging `python/rlcore/relink_rlcore.py` layout) was
      corrected to the current `relink_py/relink/cli/` structure.

## Verified done (2026-09-20, C++ relay merge)

- [x] Gave `relink_rlcore.cpp` the same `--relay`/`--nat`-implies-`--relay`
      merge as the Python build: plain-socket data-frame forwarding
      (reusing the existing header-only `relay_wire.hpp` helpers --
      `decode_relay_register`/`peek_frame_topic_id`, no new wire code)
      folded onto rlcore's own socket, gated by a `relay_groups` map
      pruned by the same TTL logic as registrations/roles.
      Decision (asked and confirmed by the user): keep the standalone
      `relink-relay` binary as-is rather than doing a full merge --
      its AF_XDP fast path (own poll loop, signal handlers, raw-frame
      parsing) would have been a much bigger, riskier port, for a
      feature the merged form was never going to have anyway. So C++
      now offers both: `relink-rlcore --relay` (plain sockets, one
      process) or the standalone `relink-relay` (only reason to still
      use it: AF_XDP). Python only has the merged form.
      Verified: full `cpp/` rebuild via CMake -- clean; `ctest` 10/10;
      `relink-rlcore --help` shows `--relay`; a raw relay REGISTER +
      data-frame test forwarded correctly between two UDP sockets
      against the real binary; ordinary registration
      (`register_client_cli`) still works unaffected while `--relay`
      is active. README updated throughout (both the quick-usage
      section and the deeper Step 13 technical section) and the
      `cpp/` directory-layout diagram in the README's file tree.

## Verified done (2026-09-20, CompressedImage + AdaptiveBitrateController)

- [x] Studied the pre-ReLink P2P video streamer at
      `/root/proj/Recom/udp-low-latency-system/src/video/*.cpp` for
      real-world JPEG compression/adaptive-quality/optimization
      technique -- it fixed-quality JPEG-encoded (turbojpeg), optionally
      picked quality from a motion-based step function (low/high
      threshold, no smoothing -- visibly flickered), downscaled before
      encoding, and stamped a timestamp on every UDP fragment (not just
      the first) for latency measurement since UDP doesn't guarantee
      order. Ported the *ideas* (motion-adaptive quality, per-chunk
      timestamp, bitrate awareness), not the code -- no turbojpeg/OpenCV
      dependency was added to the core library.
- [x] Added `AdaptiveBitrateController` (`cpp/include/relink/adaptive_bitrate.hpp`,
      `relink_py/relink/adaptive_bitrate.py`) -- pure arithmetic, no
      codec/image dependency of its own. Blends a caller-supplied motion
      score (linear quality_max..quality_min mapping against a
      configurable ceiling) with a rolling bytes/sec window clamped
      against an optional target bitrate ceiling, and exponentially
      smooths the result so quality doesn't flicker frame to frame like
      the old project's step function did.
- [x] Added a real core library type, `CompressedImage`
      (`cpp/include/relink/compressed_image.hpp`,
      `relink_py/relink/compressed_image.py`,
      `advertise_compressed_image`/`publish_compressed_image`/
      `subscribe_compressed_image` on RelinkNode in both languages) --
      same MTU-chunking as `Image`, plus a capture timestamp and the
      encoder quality actually used, stamped on EVERY chunk (not just
      chunk 0, since UDP gives no ordering guarantee) so a subscriber
      can measure true end-to-end latency and report quality with no
      side channel back to the publisher. Distinct from (and not to be
      confused with) the pre-existing ROS `sensor_msgs/CompressedImage`
      mapping onto ReLink's `Image`/`ImageChunk` -- this is a new,
      ReLink-specific wire type.
      Verified: `cpp/tests/test_compressed_image.cpp` +
      `relink_py/tests/test_compressed_image.py` (pure encode/reassemble
      round trip including OUT-OF-ORDER chunk delivery, empty image,
      dropped-chunk-never-completes, real multicast RelinkNode
      end-to-end) and `cpp/tests/test_adaptive_bitrate.cpp` +
      `relink_py/tests/test_adaptive_bitrate.py` (motion mapping,
      smoothing monotonicity, bitrate-ceiling pressure, invalid-config
      rejection) -- full `cpp/` CMake rebuild, `ctest` 12/12 (was 10),
      full Python `test_relink.py`/`test_image.py` still ALL PASS (no
      regressions from touching `node.py`'s ImageChunk special-casing
      to also cover CompressedImageChunk).
- [x] Extended `examples/camera_stream.{cpp,py}` with a third topic,
      `image_adaptive`, wired to real `AdaptiveBitrateController` +
      `CompressedImage` usage (grayscale-mean-abs-diff motion signal,
      3 Mbps bitrate ceiling), plus a `--video-file PATH` flag (loops at
      EOF) so the demo runs without camera hardware. Also fixed a
      latent bug this surfaced: the raw-topic subscriber assumed a fixed
      resolution via `reshape()`/`cv::Mat` without checking it -- since
      `cv::VideoCapture::set()`/`cap.set()` is "a request, not a
      guarantee" (the file's own prior docstring already said so), a
      camera or file that ignored it would have corrupted the reshape;
      now the publisher unconditionally resizes every frame and the
      subscriber checks the byte count before reshaping.
      Verified end-to-end against a real 1920x1080/30fps video file
      (`/root/proj/videoplayback.mp4`, provided for this test) standing
      in for a camera, in BOTH languages: Python delivered 83/83
      image_raw, 82/82 image_compressed, 82/82 image_adaptive over one
      run, with adaptive quality visibly tracking scene motion (roughly
      60s-80 range) and sub-3ms loopback latency; C++ delivered
      correctly-shaped, valid JPEGs on all three topics with sub-1ms
      latency. `libopencv-dev`/`python3-opencv` installed for this
      verification (not previously present in this environment).
- [x] Updated root README (Step 8 new "CompressedImage and adaptive
      bitrate" subsection + Review bullets, sensor_msgs footnote
      disambiguating ROS's `CompressedImage` from ReLink's own, Step 10
      `camera_stream` row, ctest count 10 -> 12), `cpp/README.md`
      (layout diagram, ctest count), and `python/relink_py/README.md`
      (examples list, tests list) to describe the new type/controller
      and the video-file testing option.

## Ideas / not started

(none open right now)

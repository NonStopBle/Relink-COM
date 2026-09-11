# relink_com_implement.md — implementation brief for Claude Code

You are implementing **ReLink-com**, a lightweight pub/sub communication
protocol (ROS-like, but faster and simpler), in C++ first. Full design
spec is in `relink-com-spec.md` — **read it in full before writing any
code**. This file is the actionable summary and build order; the spec is
the source of truth for anything this file doesn't cover in enough
detail.

Two reference files exist showing the **target API surface** you're
building toward — they will not compile until the library exists, but
they define exactly what `RelinkNode` and friends must support:
- `relink_example.cpp` — basic publisher/subscriber usage (custom type +
  default type, com-core discovery)
- `relink_benchmark.cpp` — the performance test harness you'll use to
  validate step 8 below

## What you're building (one sentence each)

- A **protocol**, not just a C++ library — byte layout must be documented
  precisely enough that Python or another language could reimplement it
  later without reading your C++ headers.
- **UDP by default**, fixed small header, `'#' ... '\n'` framed, optional
  checksum and optional AES-256-GCM encryption (both off by default, both
  deferred past the initial build — see below).
- **Discovery**: two mutually-exclusive modes — `relink-com-core` (a
  small central daemon, build this first) and multicast beacon (fully
  decentralized, build second).
- **Hard performance requirement**: 1000Hz sustained (≤1ms/message,
  judged by worst-case not average) on wired LAN. This is a floor, not a
  ceiling — expect 5,000-10,000Hz+ once properly optimized.

## Build order (follow exactly — do not reorder or parallelize prematurely)

1. **Wire format structs** — `RelinkHeader` (topic_id, seq_num,
   payload_len, flags), start/stop bytes, default std_msgs-style types
   (`Bool`/`Int8`.../`Float64`, each with a `data` field), `MultiArray<T>`
   template. Get these byte-exact per the spec (`#pragma pack`, explicit
   little-endian) before anything else.
2. **Fixed-size ring buffer** per topic, drop-oldest-on-overflow. Unit
   test the overflow behavior explicitly.
3. **UDP send/receive core, as the dedicated data thread** (see spec's
   Threading section — build the thread separation in from the start,
   don't retrofit it later). Build and test this with `secure=false`,
   `checksum=false` only — MTU check against ~1400-1450 byte budget,
   reject oversized messages at `publish()` with a clear error, never
   silently fragment.
4. **`relink-com-core`**, in **both C++ and Python** (small, isolated
   scope — see spec for why this is practical to do twice from day one).
   Default port `8445` both client and daemon side. `RegisterRequest` /
   `RegisterAck`, retry-with-backoff on the client if no ACK. Test a C++
   node against the Python daemon and vice versa — this is your proof the
   wire format is actually language-agnostic, not just claimed to be.
5. **Multicast beacon (mode B)**, only after mode A is proven — jittered
   3x-retry on startup, sparse 30-60s re-announce, topic-matching on
   receive. Confirm it produces the same peer-table outcome as mode A did
   on the same two-node test.
6. **Public API**: `node.set_com_core.ip(...)` / `.port(...)`,
   `node.use_multicast_discovery()` — **mutually exclusive, enforced at
   the setter call itself**, not deferred to `spin()`. Neither-configured
   is also an error. Track the selected mode as an explicit enum, not by
   inferring from which fields are non-empty. Write unit tests for both
   error paths (both configured, neither configured) — don't just check
   by eye.
   `advertise<T>()`/`subscribe<T>()`/`publish<T>()`, templated,
   `static_assert(std::is_trivially_copyable<T>)`, no fixed type
   registry — any struct meeting the rules just works.
7. **Correctness test**: two-process pub/sub over com-core, confirm
   delivery with zero manual peer config. Include the mutual-exclusivity
   and neither-configured error tests here too.
8. **Benchmark using `relink_benchmark.cpp`** as the harness (adapt as
   needed once the real API exists): sustained 60s run, realistic payload
   size, p50/p99/worst-case, wired LAN not localhost-only. Judge pass/fail
   against worst-case ≤1ms, not average. **Do not proceed past this step
   until it passes** — if it doesn't, profile against the Lean
   optimization checklist in the spec (allocation in hot path, blocking
   recv, lock contention) before assuming the architecture is wrong.
9. **Compare against ROS1/ROS2** at the same message size/rate on real
   target hardware, to validate the "faster than ROS" claim specifically.
10. **Apply Lean optimization techniques one at a time**, re-benchmarking
    after each, only if step 8's tail latency needs tightening — start
    with CPU pinning + lock-free ring buffer (highest leverage), not
    `recvmmsg`/socket tuning first.
11. **Stress-test discovery**: power-cycle a node mid-run, drop
    WiFi/network for a few seconds, boot 5+ nodes simultaneously.

## Do NOT implement yet (explicitly deferred — see spec for full list)

- TCP reliable transport path
- Fragmentation for messages over MTU (reject instead)
- AES-256-GCM encryption (`secure=true`) — API shape and the
  `SecureExt`/nonce design are decided in the spec (this fixes a nonce-
  reuse bug from an earlier draft — read that section carefully if/when
  you do implement this)
- Multi-language codegen (hand-written bindings only, beyond com-core's
  required C++/Python pair)
- External raw-socket client tooling beyond the wire format being
  documented

## Acceptance criteria for "v1 done"

- Steps 1-9 above complete and passing.
- `relink_benchmark.cpp`-style test shows worst-case ≤1ms sustained over
  60s on real wired LAN hardware (not localhost).
- A C++ node and a Python-run `relink-com-core` can interoperate.
- Both discovery modes produce identical downstream behavior.
- Zero heap allocation, zero locking in the data-thread hot path when
  `secure=false`/`checksum=false` (the default, benchmarked path).
- Mutual-exclusivity and neither-configured discovery config errors are
  unit tested, not just manually verified once.

If anything in this file conflicts with `relink-com-spec.md`, the spec
wins — this file is a summary and sequencing guide, not a replacement.

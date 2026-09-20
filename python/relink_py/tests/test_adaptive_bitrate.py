"""AdaptiveBitrateController tests -- pure arithmetic, no sockets, no
codec. Mirrors tests/test_adaptive_bitrate.cpp."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from relink.adaptive_bitrate import AdaptiveBitrateController

failures = 0


def check(cond, desc):
    global failures
    if cond:
        print(f"ok: {desc}")
    else:
        print(f"FAIL: {desc}", file=sys.stderr)
        failures += 1


# --- motion-based: low motion -> high quality, high motion -> low quality ---
c = AdaptiveBitrateController(quality_min=20, quality_max=80, motion_ceiling=30.0, smoothing=1.0)
check(c.next_quality(0.0, now=0.0) == 80, "zero motion maps to quality_max")
check(c.next_quality(60.0, now=0.1) == 20, "motion at/above ceiling maps to quality_min")

# --- smoothing: quality should move gradually, not jump ---
c2 = AdaptiveBitrateController(quality_min=20, quality_max=80, motion_ceiling=30.0, smoothing=0.3)
prev = c2.current_quality
check(prev == 80, "starts at quality_max")
t = 0.0
monotonic = True
for _ in range(10):
    q = c2.next_quality(60.0, now=t)
    if q > prev:
        monotonic = False
    prev = q
    t += 0.1
check(monotonic, "smoothed quality trends monotonically down toward quality_min")
check(20 <= prev < 80, "smoothed quality settles strictly below quality_max but within range")

# --- bitrate ceiling: exceeding target bitrate pulls quality down even
# with zero motion ---
c3 = AdaptiveBitrateController(quality_min=20, quality_max=80, motion_ceiling=30.0,
                                target_bitrate_bps=1_000_000, window_sec=1.0, smoothing=1.0)
t = 0.0
q = 80
for _ in range(5):
    q = c3.next_quality(0.0, now=t)
    # 200KB/frame * 5/sec = 8Mbps, way over the 1Mbps target
    c3.record_sent(200_000, now=t)
    t += 0.2
check(q < 80, "quality pulled down from the motion-only baseline of 80 under bitrate pressure")
check(q >= 20, "quality never drops below quality_min")

# --- invalid config is rejected loudly ---
threw = False
try:
    AdaptiveBitrateController(quality_min=90, quality_max=10)
except ValueError:
    threw = True
check(threw, "quality_min > quality_max is rejected")

threw = False
try:
    AdaptiveBitrateController(smoothing=0.0)
except ValueError:
    threw = True
check(threw, "smoothing of 0 is rejected (would never move)")

print()
if failures == 0:
    print("ALL PASS")
    sys.exit(0)
else:
    print(f"{failures} FAILURE(S)")
    sys.exit(1)

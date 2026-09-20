"""Adaptive bitrate controller for compressed image/video streaming --
pure arithmetic, no image/codec dependency (OpenCV, turbojpeg, etc. stay
example-level, same as relink/image.py's own "not JPEG-specific" stance;
this module doesn't even import them).

Ported from lessons learned building a real P2P video streamer before
ReLink existed: a fixed JPEG quality either wastes bandwidth on a mostly
static scene or falls behind on a busy one. This controller blends two
caller-measured signals:

  - motion: a non-negative measure of how much the scene changed since
    the last frame (e.g. mean abs pixel diff of grayscale frames -- any
    units, compared against `motion_ceiling`). More motion means less
    benefit from a high-quality encode (the next frame looks different
    anyway), so quality trends down as motion rises, up as it settles.
  - bytes actually sent for recent frames -- if the real bitrate is
    exceeding a target ceiling regardless of motion, quality is pulled
    down further, proportionally, so a busy scene can't blow through a
    bandwidth budget indefinitely.

Quality changes are exponentially smoothed so a subscriber doesn't see
frame-to-frame flicker every time motion crosses a threshold -- a naive
step function (motion > threshold ? LOW : HIGH) visibly flickers on a
scene hovering near the cutoff.
"""

import time
from typing import List, Optional, Tuple


class AdaptiveBitrateController:
    def __init__(self,
                 quality_min: int = 20,
                 quality_max: int = 80,
                 motion_ceiling: float = 30.0,
                 target_bitrate_bps: Optional[float] = None,
                 window_sec: float = 1.0,
                 smoothing: float = 0.3):
        """
        quality_min/quality_max: the JPEG-quality range (0-100) this
            controller will ever recommend.
        motion_ceiling: a motion score at or above this maps to
            quality_min; a score of 0 maps to quality_max; linear
            between the two.
        target_bitrate_bps: if set, a hard ceiling on bits/sec -- when
            the rolling average exceeds it, quality is scaled down
            further (never up past what motion already picked).
        window_sec: how long a window to average bytes/sec over.
        smoothing: exponential smoothing factor applied to the
            returned quality each call (0 = never move, 1 = no
            smoothing at all -- jumps straight to the target).
        """
        if not (0 <= quality_min <= quality_max <= 100):
            raise ValueError("expected 0 <= quality_min <= quality_max <= 100")
        if not (0.0 < smoothing <= 1.0):
            raise ValueError("smoothing must be in (0, 1]")
        self.quality_min = quality_min
        self.quality_max = quality_max
        self.motion_ceiling = max(1e-6, motion_ceiling)
        self.target_bitrate_bps = target_bitrate_bps
        self.window_sec = window_sec
        self.smoothing = smoothing

        self._quality = float(quality_max)
        self._history: List[Tuple[float, int]] = []  # (timestamp, bytes)

    def _motion_quality(self, motion: float) -> float:
        frac = max(0.0, min(1.0, motion / self.motion_ceiling))
        return self.quality_max - frac * (self.quality_max - self.quality_min)

    def _bitrate_scale(self, now: float) -> float:
        if self.target_bitrate_bps is None:
            return 1.0
        cutoff = now - self.window_sec
        self._history = [(t, b) for t, b in self._history if t >= cutoff]
        total_bytes = sum(b for _, b in self._history)
        if total_bytes <= 0:
            return 1.0
        current_bps = (total_bytes * 8.0) / self.window_sec
        if current_bps <= self.target_bitrate_bps:
            return 1.0
        return max(0.0, self.target_bitrate_bps / current_bps)

    def record_sent(self, num_bytes: int, now: Optional[float] = None):
        """Call once per frame actually sent, with its encoded size in
        bytes -- feeds the bitrate ceiling. Independent of next_quality()
        so a caller can record without necessarily calling next_quality
        every frame (e.g. a frame that was skipped, not encoded)."""
        self._history.append((now if now is not None else time.monotonic(), num_bytes))

    def next_quality(self, motion: float, now: Optional[float] = None) -> int:
        """Call once per frame, after computing `motion` for the frame
        about to be encoded. Returns the JPEG quality (in
        [quality_min, quality_max]) to use for THIS frame. Call
        record_sent() after encoding so the bitrate ceiling sees this
        frame's real size on the next call."""
        now = now if now is not None else time.monotonic()
        target = self._motion_quality(motion)
        scale = self._bitrate_scale(now)
        target = self.quality_min + (target - self.quality_min) * scale
        self._quality += (target - self._quality) * self.smoothing
        self._quality = max(self.quality_min, min(self.quality_max, self._quality))
        return int(round(self._quality))

    @property
    def current_quality(self) -> int:
        return int(round(self._quality))

// Adaptive bitrate controller for compressed image/video streaming --
// pure arithmetic, no image/codec dependency (this header does not
// include or link against any JPEG/video library -- that stays
// example-level, same as image.hpp's own "not JPEG-specific" stance).
// Mirrors relink_py/relink/adaptive_bitrate.py exactly.
//
// Ported from lessons learned building a real P2P video streamer
// before ReLink existed: a fixed JPEG quality either wastes bandwidth
// on a mostly static scene or falls behind on a busy one. This
// controller blends two caller-measured signals:
//
//   - motion: a non-negative measure of how much the scene changed
//     since the last frame (e.g. mean abs pixel diff of grayscale
//     frames -- any units, compared against `motion_ceiling`). More
//     motion means less benefit from a high-quality encode (the next
//     frame looks different anyway), so quality trends down as motion
//     rises, up as it settles.
//   - bytes actually sent for recent frames -- if the real bitrate is
//     exceeding a target ceiling regardless of motion, quality is
//     pulled down further, proportionally, so a busy scene can't blow
//     through a bandwidth budget indefinitely.
//
// Quality changes are exponentially smoothed so a subscriber doesn't
// see frame-to-frame flicker every time motion crosses a threshold --
// a naive step function (motion > threshold ? LOW : HIGH) visibly
// flickers on a scene hovering near the cutoff.

#pragma once

#include <algorithm>
#include <cstddef>
#include <deque>
#include <stdexcept>

namespace relink {

// Deliberately a free struct, not nested inside AdaptiveBitrateController:
// a nested struct's default member initializers aren't usable from a
// default *argument* of the enclosing class's own constructor (they're
// both evaluated in the enclosing class's complete-class context, which
// GCC/Clang reject as a nested dependency) -- keeping Config free of the
// class sidesteps that entirely.
struct AdaptiveBitrateConfig {
    int quality_min = 20;
    int quality_max = 80;
    double motion_ceiling = 30.0;
    // <= 0.0 means "no bitrate ceiling", motion-only.
    double target_bitrate_bps = 0.0;
    double window_sec = 1.0;
    // Exponential smoothing factor applied to the returned quality
    // each call -- must be in (0, 1]. 1.0 = no smoothing (jumps
    // straight to the target).
    double smoothing = 0.3;
};

class AdaptiveBitrateController {
public:
    using Config = AdaptiveBitrateConfig;

    explicit AdaptiveBitrateController(Config cfg = Config{}) : cfg_(cfg) {
        if (!(cfg_.quality_min >= 0 && cfg_.quality_min <= cfg_.quality_max && cfg_.quality_max <= 100)) {
            throw std::invalid_argument("expected 0 <= quality_min <= quality_max <= 100");
        }
        if (!(cfg_.smoothing > 0.0 && cfg_.smoothing <= 1.0)) {
            throw std::invalid_argument("smoothing must be in (0, 1]");
        }
        cfg_.motion_ceiling = std::max(1e-6, cfg_.motion_ceiling);
        quality_ = static_cast<double>(cfg_.quality_max);
    }

    // Call once per frame actually sent, with its encoded size in
    // bytes -- feeds the bitrate ceiling. Independent of next_quality()
    // so a caller can record without necessarily calling next_quality
    // every frame (e.g. a frame that was skipped, not encoded).
    void record_sent(std::size_t num_bytes, double now_sec) {
        history_.push_back({now_sec, num_bytes});
    }

    // Call once per frame, after computing `motion` for the frame
    // about to be encoded. Returns the JPEG quality (in
    // [quality_min, quality_max]) to use for THIS frame.
    int next_quality(double motion, double now_sec) {
        double target = motion_quality(motion);
        double scale = bitrate_scale(now_sec);
        target = cfg_.quality_min + (target - cfg_.quality_min) * scale;
        quality_ += (target - quality_) * cfg_.smoothing;
        quality_ = std::max<double>(cfg_.quality_min, std::min<double>(cfg_.quality_max, quality_));
        return static_cast<int>(quality_ + 0.5);
    }

    int current_quality() const { return static_cast<int>(quality_ + 0.5); }

private:
    struct Sample {
        double t;
        std::size_t bytes;
    };

    double motion_quality(double motion) const {
        double frac = std::max(0.0, std::min(1.0, motion / cfg_.motion_ceiling));
        return cfg_.quality_max - frac * (cfg_.quality_max - cfg_.quality_min);
    }

    double bitrate_scale(double now_sec) {
        if (cfg_.target_bitrate_bps <= 0.0) return 1.0;
        double cutoff = now_sec - cfg_.window_sec;
        while (!history_.empty() && history_.front().t < cutoff) history_.pop_front();
        std::size_t total_bytes = 0;
        for (const auto& s : history_) total_bytes += s.bytes;
        if (total_bytes == 0) return 1.0;
        double current_bps = (static_cast<double>(total_bytes) * 8.0) / cfg_.window_sec;
        if (current_bps <= cfg_.target_bitrate_bps) return 1.0;
        return std::max(0.0, cfg_.target_bitrate_bps / current_bps);
    }

    Config cfg_;
    double quality_;
    std::deque<Sample> history_;
};

} // namespace relink

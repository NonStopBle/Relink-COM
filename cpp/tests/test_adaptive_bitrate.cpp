// Step: AdaptiveBitrateController unit tests. Pure arithmetic, no
// sockets, no codec -- mirrors relink_py/tests/test_adaptive_bitrate.py.

#include "relink/adaptive_bitrate.hpp"
#include <cstdio>

using namespace relink;

static int g_failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); ++g_failures; } \
    else { std::printf("ok: %s\n", #cond); } \
} while (0)

int main() {
    // --- motion-based: low motion -> high quality, high motion -> low quality ---
    {
        AdaptiveBitrateController::Config cfg;
        cfg.quality_min = 20;
        cfg.quality_max = 80;
        cfg.motion_ceiling = 30.0;
        cfg.smoothing = 1.0; // no smoothing -- jump straight to target for this check
        AdaptiveBitrateController c(cfg);
        CHECK(c.next_quality(0.0, 0.0) == 80);
        CHECK(c.next_quality(60.0, 0.1) == 20); // >= ceiling clamps to quality_min
    }

    // --- smoothing: quality should move gradually, not jump ---
    {
        AdaptiveBitrateController::Config cfg;
        cfg.quality_min = 20;
        cfg.quality_max = 80;
        cfg.motion_ceiling = 30.0;
        cfg.smoothing = 0.3;
        AdaptiveBitrateController c(cfg);
        int prev = c.current_quality();
        CHECK(prev == 80);
        double t = 0.0;
        for (int i = 0; i < 10; ++i) {
            int q = c.next_quality(60.0, t);
            CHECK(q <= prev); // monotonically trending down toward quality_min
            prev = q;
            t += 0.1;
        }
        CHECK(prev < 80 && prev >= 20);
    }

    // --- bitrate ceiling: exceeding target bitrate pulls quality down
    // even with zero motion ---
    {
        AdaptiveBitrateController::Config cfg;
        cfg.quality_min = 20;
        cfg.quality_max = 80;
        cfg.motion_ceiling = 30.0;
        cfg.target_bitrate_bps = 1'000'000.0;
        cfg.window_sec = 1.0;
        cfg.smoothing = 1.0;
        AdaptiveBitrateController c(cfg);
        double t = 0.0;
        int q = 80;
        for (int i = 0; i < 5; ++i) {
            q = c.next_quality(0.0, t);
            // 200KB/frame * 5/sec = 8Mbps, way over the 1Mbps target
            c.record_sent(200000, t);
            t += 0.2;
        }
        CHECK(q < 80); // pulled down from the motion-only baseline of 80
        CHECK(q >= 20);
    }

    // --- invalid config is rejected loudly ---
    {
        bool threw = false;
        try {
            AdaptiveBitrateController::Config cfg;
            cfg.quality_min = 90;
            cfg.quality_max = 10; // min > max
            AdaptiveBitrateController c(cfg);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CHECK(threw);
    }

    if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
    std::printf("\n%d FAILURE(S)\n", g_failures);
    return 1;
}

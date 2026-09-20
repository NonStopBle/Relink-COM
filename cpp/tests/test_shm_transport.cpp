// Local-IPC shared-memory ring unit tests -- covers the invariants
// validated interactively during development (see the phased local-IPC
// plan): basic push/pop round trip, full-ring backpressure (not silent
// loss), zero-copy pop, and stale-segment takeover after a simulated
// crash (a leaked segment with a dead creator_pid must be detected and
// reinitialized, not blindly attached to).

#include "relink/shm_transport.hpp"
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

using namespace relink;

static int g_failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); ++g_failures; } \
    else { std::printf("ok: %s\n", #cond); } \
} while (0)

int main() {
    const char* name = "/relink_test_shm_ring";
    shm_unlink(name);  // in case a previous failed run left one behind

    // --- basic single-process round trip ---
    {
        ShmRing ring;
        CHECK(ring.open(name, 4));
        CHECK(ring.is_creator());

        uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        CHECK(ring.try_push(payload, sizeof(payload), 42));

        uint8_t out[8] = {};
        uint32_t len = 0, seq = 0;
        CHECK(ring.try_pop(out, len, seq));
        CHECK(len == sizeof(payload));
        CHECK(seq == 42);
        CHECK(std::memcmp(out, payload, sizeof(payload)) == 0);

        // empty now
        CHECK(!ring.try_pop(out, len, seq));
        ring.unlink();
    }

    // --- full-ring backpressure: never silently drops, reports false ---
    {
        ShmRing ring;
        CHECK(ring.open(name, 4));  // capacity 4 -> 3 usable slots (1 wasted for full/empty distinction)
        uint8_t payload[4] = {9, 9, 9, 9};
        int pushed = 0;
        for (int i = 0; i < 10; ++i) {
            if (ring.try_push(payload, sizeof(payload), i)) ++pushed;
        }
        CHECK(pushed == 3);  // exactly capacity-1, not silently more or fewer

        uint8_t out[4];
        uint32_t len, seq;
        int popped = 0;
        while (ring.try_pop(out, len, seq)) ++popped;
        CHECK(popped == 3);  // every accepted push is retrievable, none lost
        ring.unlink();
    }

    // --- zero-copy pop delivers the same bytes as the copying try_pop ---
    {
        ShmRing ring;
        CHECK(ring.open(name, 4));
        uint8_t payload[5] = {'h', 'e', 'l', 'l', 'o'};
        CHECK(ring.try_push(payload, sizeof(payload), 7));

        bool called = false;
        CHECK(ring.try_pop_zero_copy([&](const uint8_t* p, uint32_t len, uint32_t seq) {
            called = true;
            CHECK(len == sizeof(payload));
            CHECK(seq == 7);
            CHECK(std::memcmp(p, payload, len) == 0);
        }));
        CHECK(called);
        CHECK(!ring.try_pop_zero_copy([](const uint8_t*, uint32_t, uint32_t) {}));  // now empty
        ring.unlink();
    }

    // --- max_payload rejects an oversized push rather than truncating ---
    {
        ShmRing ring;
        CHECK(ring.open(name, 4, /*max_payload=*/8));
        uint8_t big[9] = {};
        CHECK(!ring.try_push(big, sizeof(big), 0));  // 9 > max_payload(8)
        uint8_t ok_size[8] = {};
        CHECK(ring.try_push(ok_size, sizeof(ok_size), 0));  // exactly at the limit
        ring.unlink();
    }

    // --- stale-segment takeover: a creator that crashes (SIGKILL, no
    // unlink) leaves a segment behind; the next opener must detect the
    // dead creator_pid and reinitialize rather than attach to garbage
    // head/tail state. ---
    {
        shm_unlink(name);
        pid_t child = fork();
        if (child == 0) {
            ShmRing ring;
            if (!ring.open(name, 4)) _exit(1);
            uint8_t payload[4] = {1, 2, 3, 4};
            ring.try_push(payload, sizeof(payload), 99);  // leaves tail advanced, non-zero state
            // Deliberately no unlink() -- simulate a crash.
            raise(SIGKILL);
            _exit(0);  // unreachable
        }
        int status = 0;
        waitpid(child, &status, 0);
        CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);  // confirm it really "crashed"

        // Segment must still exist in /dev/shm (nothing cleaned it up).
        int fd = shm_open(name, O_RDONLY, 0666);
        CHECK(fd >= 0);
        if (fd >= 0) ::close(fd);

        // A fresh open() must detect the dead pid, steal, and reset to
        // a clean empty ring -- not silently attach to the stale
        // tail=1 state and let head/tail desync.
        ShmRing ring2;
        CHECK(ring2.open(name, 4));
        CHECK(ring2.is_creator());  // it had to steal creator role
        uint8_t out[4]; uint32_t len, seq;
        CHECK(!ring2.try_pop(out, len, seq));  // reset to empty, not inheriting the crashed push
        ring2.unlink();
    }

    if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
    std::printf("\n%d FAILURE(S)\n", g_failures);
    return 1;
}

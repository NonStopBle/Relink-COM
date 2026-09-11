// Step 2 unit tests: RingBuffer<T,N> — capacity, FIFO order, and
// explicit drop-oldest-on-overflow behavior (spec: "on overflow, drop
// the oldest message, never block the publisher").

#include "relink/ring_buffer.hpp"
#include <cstdio>

using namespace relink;

static int g_failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
        ++g_failures; \
    } else { \
        std::printf("ok: %s\n", #cond); \
    } \
} while (0)

int main() {
    // --- basic push/pop, FIFO order, empty/full state ---
    {
        RingBuffer<int, 4> rb;
        CHECK(rb.empty());
        CHECK(!rb.full());
        CHECK(rb.capacity() == 4);
        CHECK(rb.size() == 0);

        CHECK(rb.push(10) == false); // no drop, still room
        CHECK(rb.push(20) == false);
        CHECK(rb.size() == 2);
        CHECK(!rb.empty());
        CHECK(!rb.full());

        auto v1 = rb.pop();
        CHECK(v1.has_value());
        CHECK(*v1 == 10); // FIFO: oldest first
        auto v2 = rb.pop();
        CHECK(v2.has_value());
        CHECK(*v2 == 20);
        CHECK(rb.empty());

        auto v3 = rb.pop();
        CHECK(!v3.has_value()); // pop on empty returns nullopt
    }

    // --- fill exactly to capacity, no overflow yet ---
    {
        RingBuffer<int, 3> rb;
        CHECK(rb.push(1) == false);
        CHECK(rb.push(2) == false);
        CHECK(rb.push(3) == false);
        CHECK(rb.full());
        CHECK(rb.size() == 3);
    }

    // --- overflow: drop-oldest behavior, explicit ---
    {
        RingBuffer<int, 3> rb;
        rb.push(1);
        rb.push(2);
        rb.push(3);
        CHECK(rb.full());

        // pushing a 4th element while full must drop the oldest (1),
        // never block, and report dropped == true
        bool dropped = rb.push(4);
        CHECK(dropped == true);
        CHECK(rb.full());          // still full, size unchanged at capacity
        CHECK(rb.size() == 3);

        // remaining contents must be [2, 3, 4] in FIFO order -- 1 is gone
        auto a = rb.pop(); CHECK(a.has_value()); CHECK(*a == 2);
        auto b = rb.pop(); CHECK(b.has_value()); CHECK(*b == 3);
        auto c = rb.pop(); CHECK(c.has_value()); CHECK(*c == 4);
        CHECK(rb.empty());
    }

    // --- repeated overflow: sustained drop-oldest over many pushes ---
    {
        RingBuffer<int, 5> rb;
        for (int i = 0; i < 100; ++i) {
            rb.push(i);
        }
        // buffer should hold exactly the last 5 values: 95..99
        CHECK(rb.full());
        CHECK(rb.size() == 5);
        for (int expected = 95; expected < 100; ++expected) {
            auto v = rb.pop();
            CHECK(v.has_value());
            CHECK(*v == expected);
        }
        CHECK(rb.empty());
    }

    // --- clear() resets state ---
    {
        RingBuffer<int, 4> rb;
        rb.push(1); rb.push(2);
        rb.clear();
        CHECK(rb.empty());
        CHECK(rb.size() == 0);
        CHECK(rb.push(99) == false); // fresh state, no drop
        auto v = rb.pop();
        CHECK(v.has_value());
        CHECK(*v == 99);
    }

    // --- trivially-copyable wire struct works, not just int ---
    {
        struct Small { int a; float b; };
        static_assert(std::is_trivially_copyable<Small>::value, "");
        RingBuffer<Small, 2> rb;
        rb.push(Small{1, 2.5f});
        rb.push(Small{2, 3.5f});
        CHECK(rb.push(Small{3, 4.5f}) == true); // drops {1,2.5f}
        auto v = rb.pop();
        CHECK(v.has_value());
        CHECK(v->a == 2);
    }

    if (g_failures == 0) {
        std::printf("\nALL PASS\n");
        return 0;
    } else {
        std::printf("\n%d FAILURE(S)\n", g_failures);
        return 1;
    }
}

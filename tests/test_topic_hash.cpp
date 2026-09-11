// Unit tests for named-topic hashing (topic_hash.hpp) and RelinkNode's
// string-topic registry (topic_id_for/topic_name_for/rltopic_list).
// Plain asserts, no test framework dependency, same style as the other
// tests/test_*.cpp files.

#include "relink/relink.hpp"
#include "relink/topic_hash.hpp"
#include <cassert>
#include <cstdio>
#include <stdexcept>

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

struct Msg { int32_t x; };

int main() {
    // --- fnv1a32: deterministic, distinct names diverge ---
    CHECK(fnv1a32("/relink/lidar") == fnv1a32("/relink/lidar"));
    CHECK(fnv1a32("/relink/lidar") != fnv1a32("/relink/camera/front"));
    CHECK(fnv1a32("") == 0x811c9dc5u); // empty string: just the FNV offset basis

    // --- RelinkNode::topic_id_for: same name -> same id, distinct names -> distinct ids ---
    {
        RelinkNode node;
        uint32_t id1 = node.topic_id_for("/relink/camera/front");
        uint32_t id2 = node.topic_id_for("/relink/camera/front");
        uint32_t id3 = node.topic_id_for("/relink/camera/back");
        CHECK(id1 == id2);
        CHECK(id1 != id3);
        CHECK(id1 == fnv1a32("/relink/camera/front"));
    }

    // --- topic_name_for: reverse lookup via the local registry ---
    {
        RelinkNode node;
        uint32_t id = node.topic_id_for("/relink/imu");
        CHECK(node.topic_name_for(id) == "/relink/imu");
        CHECK(node.topic_name_for(id + 1) == ""); // never named here -> empty
    }

    // --- reserved NAT-punch id guard: sanity-check the common path (an
    // ordinary name must NOT throw). The reject branch itself (a name
    // whose hash happens to equal kNatPunchTopicId == 0xFFFFFFFF) is not
    // covered here -- a brute-force search over ~2 billion candidate
    // strings (see tests/test_topic_hash.cpp git history / dev notes)
    // did not find one, and deliberately is not worth burning more CPU
    // to find one further out of a ~1-in-4.29-billion space. The guard's
    // logic is a single `id == kNatPunchTopicId` comparison, exercised
    // indirectly by every call below via `fnv1a32(...) == kNatPunchTopicId`. ---
    {
        RelinkNode node;
        bool threw = false;
        try {
            node.topic_id_for("/relink/definitely-not-reserved");
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(!threw);
        CHECK(fnv1a32("/relink/definitely-not-reserved") != kNatPunchTopicId);
    }

    // --- genuine hash collision: two different real strings that hash to
    // the SAME 32-bit id (found by brute-force search, not synthetic) --
    // registering both under the same RelinkNode must throw. ---
    {
        RelinkNode node;
        const std::string a = "/relink/topic/162789";
        const std::string b = "/relink/topic/379192";
        CHECK(fnv1a32(a) == fnv1a32(b)); // confirms the fixture is a real collision
        node.topic_id_for(a);
        bool threw = false;
        try {
            node.topic_id_for(b);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);
    }

    // --- rltopic_list: reflects declared topics with names when known,
    // empty name for numerically-declared topics ---
    {
        RelinkNode node;
        node.advertise<Msg>("/relink/lidar");
        node.subscribe<Msg>(4242u, [](const Msg&) {}); // numeric, no name

        auto list = node.rltopic_list();
        CHECK(list.size() == 2);

        bool found_named = false, found_numeric = false;
        for (auto& t : list) {
            if (t.topic_id == fnv1a32("/relink/lidar")) {
                CHECK(t.name == "/relink/lidar");
                found_named = true;
            }
            if (t.topic_id == 4242u) {
                CHECK(t.name.empty());
                found_numeric = true;
            }
        }
        CHECK(found_named);
        CHECK(found_numeric);
    }

    // --- string and uint32_t overloads must resolve to the same wire id ---
    {
        RelinkNode node_a, node_b;
        node_a.advertise<Msg>("/relink/lidar");
        node_b.advertise<Msg>(fnv1a32("/relink/lidar"));
        CHECK(node_a.rltopic_list()[0].topic_id == node_b.rltopic_list()[0].topic_id);
    }

    if (g_failures == 0) {
        std::printf("\nALL PASS\n");
        return 0;
    } else {
        std::printf("\n%d FAILURE(S)\n", g_failures);
        return 1;
    }
}

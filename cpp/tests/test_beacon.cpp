// Step 5 pure codec tests: BeaconPacket encode/decode round trip.

#include "relink/beacon.hpp"
#include <cstdio>

using namespace relink;

static int g_failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); ++g_failures; } \
    else { std::printf("ok: %s\n", #cond); } \
} while (0)

int main() {
    {
        uint32_t topics[2] = {100, 200};
        uint8_t buf[64];
        size_t len = 0;
        auto er = encode_beacon_packet(0x0A000005, 5000, topics, 2, buf, sizeof(buf), &len,
                                        /*node_pid=*/4242, /*shm_capable=*/true);
        CHECK(er == BeaconEncodeResult::Ok);
        // +8 for the Phase 5 node_pid/capability_flags extension, always
        // appended on encode (see beacon.hpp's header comment).
        CHECK(len == sizeof(BeaconPacketHeader) + 2 * sizeof(uint32_t) + 8);

        DecodedBeacon b{};
        CHECK(decode_beacon_packet(buf, len, &b) == BeaconDecodeResult::Ok);
        CHECK(b.node_ip == 0x0A000005);
        CHECK(b.node_port == 5000);
        CHECK(b.topic_count == 2);
        CHECK(beacon_topic_at(b, 0) == 100);
        CHECK(beacon_topic_at(b, 1) == 200);
        CHECK(b.node_pid == 4242);
        CHECK(b.shm_capable == true);
    }
    {
        // A beacon WITHOUT the extension (old-format-shaped buffer, or
        // one truncated exactly at the extension boundary) still
        // decodes fine -- pid/shm_capable just default to 0/false. This
        // is the backward-compatibility case the ">=" length check
        // (instead of the original "==") exists for.
        uint32_t topics[2] = {100, 200};
        uint8_t buf[64];
        size_t len = 0;
        encode_beacon_packet(0x0A000005, 5000, topics, 2, buf, sizeof(buf), &len);
        size_t old_format_len = sizeof(BeaconPacketHeader) + 2 * sizeof(uint32_t);
        DecodedBeacon b{};
        CHECK(decode_beacon_packet(buf, old_format_len, &b) == BeaconDecodeResult::Ok);
        CHECK(b.node_pid == 0);
        CHECK(b.shm_capable == false);
        CHECK(b.topic_count == 2);  // topic list itself is still intact
    }
    {
        uint8_t buf[64];
        size_t len = 0;
        auto er = encode_beacon_packet(1, 2, nullptr, 0, buf, sizeof(buf), &len);
        CHECK(er == BeaconEncodeResult::Ok);
        DecodedBeacon b{};
        CHECK(decode_beacon_packet(buf, len, &b) == BeaconDecodeResult::Ok);
        CHECK(b.topic_count == 0);
    }
    {
        // Truncating into the TOPIC data itself (not just the optional
        // trailing extension) must still be rejected -- the ">=" length
        // check only tolerates a missing/short extension, not a short
        // topic list.
        uint8_t buf[64];
        size_t len = 0;
        uint32_t topics[1] = {9};
        encode_beacon_packet(1, 2, topics, 1, buf, sizeof(buf), &len);
        size_t topic_data_end = sizeof(BeaconPacketHeader) + 1 * sizeof(uint32_t);
        DecodedBeacon b{};
        CHECK(decode_beacon_packet(buf, topic_data_end - 1, &b) == BeaconDecodeResult::LengthMismatch);
    }

    if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
    std::printf("\n%d FAILURE(S)\n", g_failures);
    return 1;
}

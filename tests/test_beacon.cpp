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
        uint16_t topics[2] = {100, 200};
        uint8_t buf[64];
        size_t len = 0;
        auto er = encode_beacon_packet(0x0A000005, 5000, topics, 2, buf, sizeof(buf), &len);
        CHECK(er == BeaconEncodeResult::Ok);
        CHECK(len == sizeof(BeaconPacketHeader) + 2 * sizeof(uint16_t));

        DecodedBeacon b{};
        CHECK(decode_beacon_packet(buf, len, &b) == BeaconDecodeResult::Ok);
        CHECK(b.node_ip == 0x0A000005);
        CHECK(b.node_port == 5000);
        CHECK(b.topic_count == 2);
        CHECK(beacon_topic_at(b, 0) == 100);
        CHECK(beacon_topic_at(b, 1) == 200);
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
        uint8_t buf[64];
        size_t len = 0;
        uint16_t topics[1] = {9};
        encode_beacon_packet(1, 2, topics, 1, buf, sizeof(buf), &len);
        DecodedBeacon b{};
        CHECK(decode_beacon_packet(buf, len - 1, &b) == BeaconDecodeResult::LengthMismatch);
    }

    if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
    std::printf("\n%d FAILURE(S)\n", g_failures);
    return 1;
}

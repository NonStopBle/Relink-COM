// Step 4 pure codec tests: RegisterRequest/RegisterAck encode/decode
// round trip, no sockets.

#include "relink/register.hpp"
#include <cstdio>

using namespace relink;

static int g_failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); ++g_failures; } \
    else { std::printf("ok: %s\n", #cond); } \
} while (0)

int main() {
    // --- RegisterRequest round trip ---
    {
        uint32_t topics[3] = {100, 101, 200};
        uint8_t buf[256];
        size_t len = 0;
        auto er = encode_register_request(0x0A000005, 5555, topics, 3, buf, sizeof(buf), &len);
        CHECK(er == RegisterEncodeResult::Ok);
        CHECK(len == sizeof(RegisterRequestHeader) + 3 * sizeof(uint32_t));

        DecodedRegisterRequest dreq{};
        auto dr = decode_register_request(buf, len, &dreq);
        CHECK(dr == RegisterDecodeResult::Ok);
        CHECK(dreq.node_ip == 0x0A000005);
        CHECK(dreq.node_port == 5555);
        CHECK(dreq.topic_count == 3);
        CHECK(register_request_topic_at(dreq, 0) == 100);
        CHECK(register_request_topic_at(dreq, 1) == 101);
        CHECK(register_request_topic_at(dreq, 2) == 200);
    }

    // --- RegisterAck round trip ---
    {
        RegisterAckPeer peers[2] = {
            {0x0A000006, 6000, 100, 0xC0A80006, 6000},  // has a LAN candidate
            {0x0A000007, 7000, 101, 0, 0},              // no LAN candidate known
        };
        uint8_t buf[256];
        size_t len = 0;
        auto er = encode_register_ack(0, peers, 2, buf, sizeof(buf), &len);
        CHECK(er == RegisterEncodeResult::Ok);

        DecodedRegisterAck dack{};
        auto dr = decode_register_ack(buf, len, &dack);
        CHECK(dr == RegisterDecodeResult::Ok);
        CHECK(dack.status == 0);
        CHECK(dack.peer_count == 2);
        auto p0 = register_ack_peer_at(dack, 0);
        CHECK(p0.ip == 0x0A000006);
        CHECK(p0.port == 6000);
        CHECK(p0.topic_id == 100);
        CHECK(p0.lan_ip == 0xC0A80006);
        CHECK(p0.lan_port == 6000);
        auto p1 = register_ack_peer_at(dack, 1);
        CHECK(p1.ip == 0x0A000007);
        CHECK(p1.topic_id == 101);
        CHECK(p1.lan_ip == 0);
        CHECK(p1.lan_port == 0);
    }

    // --- empty topic list / empty peer list ---
    {
        uint8_t buf[64];
        size_t len = 0;
        auto er = encode_register_request(1, 2, nullptr, 0, buf, sizeof(buf), &len);
        CHECK(er == RegisterEncodeResult::Ok);
        CHECK(len == sizeof(RegisterRequestHeader));

        DecodedRegisterRequest dreq{};
        CHECK(decode_register_request(buf, len, &dreq) == RegisterDecodeResult::Ok);
        CHECK(dreq.topic_count == 0);
    }

    // --- length mismatch rejected ---
    {
        uint8_t buf[64];
        size_t len = 0;
        uint32_t topics[1] = {5};
        encode_register_request(1, 2, topics, 1, buf, sizeof(buf), &len);
        DecodedRegisterRequest dreq{};
        CHECK(decode_register_request(buf, len - 1, &dreq) == RegisterDecodeResult::LengthMismatch);
    }

    if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
    std::printf("\n%d FAILURE(S)\n", g_failures);
    return 1;
}

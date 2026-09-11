// Step 3 unit tests (pure, no sockets): frame encode/decode round trip,
// MTU rejection, and malformed-frame rejection paths.

#include "relink/frame.hpp"
#include <cstdio>
#include <cstring>

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
    // --- round trip: encode then decode recovers the same fields ---
    {
        struct Msg { float a; uint64_t b; };
        Msg msg{3.14f, 123456789ULL};

        uint8_t buf[kMaxFrameBytes];
        size_t len = 0;
        EncodeResult er = encode_frame(/*topic*/42, /*seq*/7, &msg, sizeof(msg), buf, sizeof(buf), &len);
        CHECK(er == EncodeResult::Ok);
        CHECK(len == 1 + sizeof(RelinkHeader) + sizeof(msg) + 1);
        CHECK(buf[0] == kStartByte);
        CHECK(buf[len - 1] == kStopByte);

        DecodedFrame decoded{};
        DecodeResult dr = decode_frame(buf, len, &decoded);
        CHECK(dr == DecodeResult::Ok);
        CHECK(decoded.header.topic_id == 42);
        CHECK(decoded.header.seq_num == 7);
        CHECK(decoded.payload_len == sizeof(msg));

        Msg out{};
        std::memcpy(&out, decoded.payload, sizeof(out));
        CHECK(out.a == msg.a);
        CHECK(out.b == msg.b);
    }

    // --- zero-length payload is valid ---
    {
        uint8_t buf[kMaxFrameBytes];
        size_t len = 0;
        EncodeResult er = encode_frame(1, 0, nullptr, 0, buf, sizeof(buf), &len);
        CHECK(er == EncodeResult::Ok);
        CHECK(len == 1 + sizeof(RelinkHeader) + 1);

        DecodedFrame decoded{};
        DecodeResult dr = decode_frame(buf, len, &decoded);
        CHECK(dr == DecodeResult::Ok);
        CHECK(decoded.payload_len == 0);
    }

    // --- payload containing raw 0x0A and 0x23 bytes must not confuse
    //     the decoder into stopping early (spec's "never scan" rule) ---
    {
        uint8_t payload[5] = { 0x0A, 0x23, 0x0A, 0x00, 0x23 };
        uint8_t buf[kMaxFrameBytes];
        size_t len = 0;
        EncodeResult er = encode_frame(9, 1, payload, sizeof(payload), buf, sizeof(buf), &len);
        CHECK(er == EncodeResult::Ok);

        DecodedFrame decoded{};
        DecodeResult dr = decode_frame(buf, len, &decoded);
        CHECK(dr == DecodeResult::Ok);
        CHECK(decoded.payload_len == sizeof(payload));
        CHECK(std::memcmp(decoded.payload, payload, sizeof(payload)) == 0);
    }

    // --- oversized payload rejected at encode, not truncated ---
    {
        static uint8_t big[kMaxPayloadBytes + 1] = {};
        uint8_t buf[kMaxFrameBytes];
        size_t len = 0;
        EncodeResult er = encode_frame(1, 0, big, sizeof(big), buf, sizeof(buf), &len);
        CHECK(er == EncodeResult::PayloadTooLarge);
    }

    // --- exactly at the MTU budget succeeds ---
    {
        static uint8_t exact[kMaxPayloadBytes] = {};
        uint8_t buf[kMaxFrameBytes];
        size_t len = 0;
        EncodeResult er = encode_frame(1, 0, exact, sizeof(exact), buf, sizeof(buf), &len);
        CHECK(er == EncodeResult::Ok);
    }

    // --- malformed frames rejected on decode ---
    {
        uint8_t too_short[3] = {kStartByte, 0, 0};
        DecodedFrame decoded{};
        CHECK(decode_frame(too_short, sizeof(too_short), &decoded) == DecodeResult::TooShort);
    }
    {
        uint8_t buf[kMaxFrameBytes];
        size_t len = 0;
        encode_frame(1, 0, nullptr, 0, buf, sizeof(buf), &len);
        buf[0] = 0xFF; // corrupt start byte
        DecodedFrame decoded{};
        CHECK(decode_frame(buf, len, &decoded) == DecodeResult::BadStartByte);
    }
    {
        uint8_t buf[kMaxFrameBytes];
        size_t len = 0;
        encode_frame(1, 0, nullptr, 0, buf, sizeof(buf), &len);
        buf[len - 1] = 0xFF; // corrupt stop byte
        DecodedFrame decoded{};
        CHECK(decode_frame(buf, len, &decoded) == DecodeResult::BadStopByte);
    }
    {
        // payload_len in header claims more bytes than the buffer holds
        uint8_t payload[4] = {1, 2, 3, 4};
        uint8_t buf[kMaxFrameBytes];
        size_t len = 0;
        encode_frame(1, 0, payload, sizeof(payload), buf, sizeof(buf), &len);
        RelinkHeader h{};
        std::memcpy(&h, buf + 1, sizeof(h));
        h.payload_len = 200; // lie about length
        std::memcpy(buf + 1, &h, sizeof(h));
        DecodedFrame decoded{};
        CHECK(decode_frame(buf, len, &decoded) == DecodeResult::LengthMismatch);
    }

    if (g_failures == 0) {
        std::printf("\nALL PASS\n");
        return 0;
    } else {
        std::printf("\n%d FAILURE(S)\n", g_failures);
        return 1;
    }
}

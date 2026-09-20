// Step: CompressedImage type unit + real-socket tests. Mirrors
// test_image.cpp, plus checks the two extra fields (capture timestamp,
// quality) survive out-of-order chunk delivery.

#include "relink/compressed_image.hpp"
#include "relink/relink.hpp"
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
#include <random>
#include <atomic>
#include <chrono>
#include <thread>
#include <mutex>

using namespace relink;

static int g_failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); ++g_failures; } \
    else { std::printf("ok: %s\n", #cond); } \
} while (0)

int main() {
    // --- CompressedImageChunk fills the MTU budget exactly ---
    CHECK(sizeof(CompressedImageChunk) <= kMaxPayloadBytes);
    CHECK(kCompressedImageChunkDataBytes == kMaxPayloadBytes - kCompressedImageChunkHeaderBytes);
    CHECK(kCompressedImageChunkHeaderBytes == 19);

    // --- pure encode/reassemble round trip, no sockets ---
    {
        std::vector<uint8_t> data(5000);
        for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i);

        std::vector<CompressedImageChunk> chunks;
        auto result = encode_compressed_image_chunks(42, data.data(), data.size(),
                                                       123456789ULL, 37,
                                                       [&](const CompressedImageChunk& c) { chunks.push_back(c); });
        CHECK(result == CompressedImageEncodeResult::Ok);
        size_t expected_chunks = (data.size() + kCompressedImageChunkDataBytes - 1) / kCompressedImageChunkDataBytes;
        CHECK(chunks.size() == expected_chunks);
        CHECK(chunks.front().frame_id == 42);
        CHECK(chunks.back().chunk_index == chunks.size() - 1);
        for (const auto& c : chunks) {
            CHECK(c.capture_timestamp_ns == 123456789ULL);
            CHECK(c.quality == 37);
        }

        // Deliver out of order -- UDP gives no ordering guarantee, and
        // the timestamp/quality live on every chunk precisely so this
        // still works even if chunk 0 isn't first to arrive.
        std::vector<CompressedImageChunk> shuffled = chunks;
        std::mt19937 rng(1234);
        std::shuffle(shuffled.begin(), shuffled.end(), rng);

        bool completed = false;
        std::vector<uint8_t> reassembled;
        uint64_t got_ts = 0;
        uint8_t got_q = 0;
        CompressedImageReassembler reassembler(
            [&](uint32_t frame_id, const std::vector<uint8_t>& img, uint64_t ts, uint8_t q) {
                CHECK(frame_id == 42);
                completed = true;
                reassembled = img;
                got_ts = ts;
                got_q = q;
            });
        for (const auto& c : shuffled) reassembler.on_chunk(c);
        CHECK(completed);
        CHECK(reassembled == data);
        CHECK(got_ts == 123456789ULL);
        CHECK(got_q == 37);
    }

    // --- empty image (zero bytes) still round-trips as one empty chunk ---
    {
        std::vector<CompressedImageChunk> chunks;
        encode_compressed_image_chunks(1, nullptr, 0, 0, 0,
                                        [&](const CompressedImageChunk& c) { chunks.push_back(c); });
        CHECK(chunks.size() == 1);
        CHECK(chunks[0].chunk_bytes == 0);

        bool completed = false;
        CompressedImageReassembler reassembler(
            [&](uint32_t, const std::vector<uint8_t>& img, uint64_t, uint8_t) {
                completed = true;
                CHECK(img.empty());
            });
        reassembler.on_chunk(chunks[0]);
        CHECK(completed);
    }

    // --- a dropped chunk means the frame never completes, and a new
    // frame_id starting resets the reassembler (no stale partial data) ---
    {
        std::vector<uint8_t> data(5000, 0xAB);
        std::vector<CompressedImageChunk> chunks;
        encode_compressed_image_chunks(7, data.data(), data.size(), 111, 50,
                                        [&](const CompressedImageChunk& c) { chunks.push_back(c); });
        CHECK(chunks.size() >= 3);

        int complete_count = 0;
        CompressedImageReassembler reassembler(
            [&](uint32_t, const std::vector<uint8_t>&, uint64_t, uint8_t) { ++complete_count; });
        reassembler.on_chunk(chunks[0]);
        // skip chunks[1] -- simulates a dropped datagram
        for (size_t i = 2; i < chunks.size(); ++i) reassembler.on_chunk(chunks[i]);
        CHECK(complete_count == 0); // never completed, correctly

        std::vector<uint8_t> data2(3000, 0xCD);
        std::vector<CompressedImageChunk> chunks2;
        encode_compressed_image_chunks(8, data2.data(), data2.size(), 222, 60,
                                        [&](const CompressedImageChunk& c) { chunks2.push_back(c); });
        for (const auto& c : chunks2) reassembler.on_chunk(c);
        CHECK(complete_count == 1);
    }

    // --- real end-to-end over RelinkNode via multicast ---
    {
        RelinkNode pub, sub;
        pub.use_multicast_discovery();
        sub.use_multicast_discovery();

        std::atomic<bool> got{false};
        std::mutex mtx;
        std::vector<uint8_t> received_image;
        uint64_t received_ts = 0;
        uint8_t received_q = 0;

        sub.subscribe_compressed_image(601, [&](uint32_t, const std::vector<uint8_t>& img,
                                                 uint64_t ts, uint8_t q) {
            std::lock_guard<std::mutex> lock(mtx);
            received_image = img;
            received_ts = ts;
            received_q = q;
            got = true;
        });
        pub.advertise_compressed_image(601);

        std::thread t1([&] { sub.spin_once(); });
        std::thread t2([&] { pub.spin_once(); });
        t1.join(); t2.join();

        std::this_thread::sleep_for(std::chrono::milliseconds(1500));

        std::vector<uint8_t> big_data(10000);
        for (size_t i = 0; i < big_data.size(); ++i) big_data[i] = static_cast<uint8_t>(i * 3);
        bool sent = pub.publish_compressed_image(601, big_data.data(), big_data.size(),
                                                  /*quality=*/72, /*frame_id=*/99,
                                                  /*capture_timestamp_ns=*/987654321ULL);
        CHECK(sent);

        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!got.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        CHECK(got.load());
        std::lock_guard<std::mutex> lock(mtx);
        CHECK(received_image == big_data);
        CHECK(received_ts == 987654321ULL);
        CHECK(received_q == 72);
    }

    if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
    std::printf("\n%d FAILURE(S)\n", g_failures);
    return 1;
}

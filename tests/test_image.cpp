// Step: Image type unit + real-socket tests. Mirrors the style of the
// other tests/test_*.cpp files.

#include "relink/image.hpp"
#include "relink/relink.hpp"
#include <cstdio>
#include <cstring>
#include <vector>
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
    // --- ImageChunk fills the MTU budget exactly ---
    CHECK(sizeof(ImageChunk) <= kMaxPayloadBytes);
    CHECK(kImageChunkDataBytes == kMaxPayloadBytes - kImageChunkHeaderBytes);

    // --- pure encode/reassemble round trip, no sockets ---
    {
        std::vector<uint8_t> data(5000);
        for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i);

        std::vector<ImageChunk> chunks;
        auto result = encode_image_chunks(42, data.data(), data.size(),
                                           [&](const ImageChunk& c) { chunks.push_back(c); });
        CHECK(result == ImageEncodeResult::Ok);
        size_t expected_chunks = (data.size() + kImageChunkDataBytes - 1) / kImageChunkDataBytes;
        CHECK(chunks.size() == expected_chunks);
        CHECK(chunks.front().frame_id == 42);
        CHECK(chunks.back().chunk_index == chunks.size() - 1);

        bool completed = false;
        std::vector<uint8_t> reassembled;
        ImageReassembler reassembler([&](uint32_t frame_id, const std::vector<uint8_t>& img) {
            CHECK(frame_id == 42);
            completed = true;
            reassembled = img;
        });
        for (const auto& c : chunks) reassembler.on_chunk(c);
        CHECK(completed);
        CHECK(reassembled == data);
    }

    // --- empty image (zero bytes) still round-trips as one empty chunk ---
    {
        std::vector<ImageChunk> chunks;
        encode_image_chunks(1, nullptr, 0, [&](const ImageChunk& c) { chunks.push_back(c); });
        CHECK(chunks.size() == 1);
        CHECK(chunks[0].chunk_bytes == 0);

        bool completed = false;
        ImageReassembler reassembler([&](uint32_t, const std::vector<uint8_t>& img) {
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
        std::vector<ImageChunk> chunks;
        encode_image_chunks(7, data.data(), data.size(), [&](const ImageChunk& c) { chunks.push_back(c); });
        CHECK(chunks.size() >= 3);

        int complete_count = 0;
        ImageReassembler reassembler([&](uint32_t, const std::vector<uint8_t>&) { ++complete_count; });
        reassembler.on_chunk(chunks[0]);
        // skip chunks[1] -- simulates a dropped datagram
        for (size_t i = 2; i < chunks.size(); ++i) reassembler.on_chunk(chunks[i]);
        CHECK(complete_count == 0); // never completed, correctly

        // a fresh frame_id's chunks now arrive -- must reassemble cleanly
        std::vector<uint8_t> data2(3000, 0xCD);
        std::vector<ImageChunk> chunks2;
        encode_image_chunks(8, data2.data(), data2.size(), [&](const ImageChunk& c) { chunks2.push_back(c); });
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

        sub.subscribe_image(600, [&](uint32_t, const std::vector<uint8_t>& img) {
            std::lock_guard<std::mutex> lock(mtx);
            received_image = img;
            got = true;
        });
        pub.advertise_image(600);

        // Simultaneous ensure_started() via spin_once(), matching the
        // pattern proven to work reliably in earlier discovery tests.
        std::thread t1([&] { sub.spin_once(); });
        std::thread t2([&] { pub.spin_once(); });
        t1.join(); t2.join();

        std::this_thread::sleep_for(std::chrono::milliseconds(1500));

        std::vector<uint8_t> big_data(10000);
        for (size_t i = 0; i < big_data.size(); ++i) big_data[i] = static_cast<uint8_t>(i * 3);
        bool sent = pub.publish_image(600, big_data.data(), big_data.size());
        CHECK(sent);

        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!got.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        CHECK(got.load());
        std::lock_guard<std::mutex> lock(mtx);
        CHECK(received_image == big_data);
    }

    if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
    std::printf("\n%d FAILURE(S)\n", g_failures);
    return 1;
}

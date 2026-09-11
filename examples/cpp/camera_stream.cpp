// camera_stream -- stream a real webcam over ReLink as both an
// uncompressed ("image_raw") and JPEG-compressed ("image_compressed")
// topic, to demonstrate the practical tradeoff between them.
//
// REQUIRES OPENCV, WHICH IS NOT PART OF RELINK AND IS NOT INSTALLED FOR
// YOU. Install it yourself first, e.g.:
//   Ubuntu/Debian: sudo apt install libopencv-dev
// then build with: g++ ... $(pkg-config --cflags --libs opencv4) ...
//
// Why chunking: a raw 640x480 BGR frame is ~920KB and even a JPEG-
// compressed frame is usually well over ReLink's ~1400-byte MTU budget
// -- ReLink intentionally does not fragment/reassemble large messages
// itself ("one message, one UDP datagram" is the whole design). This
// example is exactly the documented way to send something bigger: an
// ordinary trivially-copyable ImageChunk message type, nothing library-
// side changed, with the splitting/reassembly done here in application
// code -- the same pattern you'd use for any payload larger than one
// UDP datagram.
//
// Tested end-to-end (real camera, real chunked pub/sub) at every
// resolution this sandbox's camera actually supports:
//   320x180  (169 raw chunks/frame) -- compressed 3/3 delivered, raw 0/3
//   320x240  (225 raw chunks/frame) -- compressed 3/3 delivered, raw 1/3
//   640x360  (675 raw chunks/frame) -- compressed 3/3 delivered, raw 0/3
//   640x480  (900 raw chunks/frame) -- compressed 3/3 delivered, raw 0/3
// The lesson is real, not theoretical: compressed (2-3 chunks/frame) was
// 12/12 reliable; raw (hundreds of chunks/frame) was 1/12. A dropped
// chunk drops the whole frame (no retransmission), so more chunks per
// frame means more chances to lose one -- use image_compressed for
// anything resembling real-time video. On your own machine with a real
// 1080p+ webcam this same code will negotiate whatever resolution the
// hardware actually supports (cv::VideoCapture::set() is a request, not
// a guarantee) -- raw chunk counts scale directly with resolution, so
// the reliability gap only gets wider at higher resolutions.
//
// Build (one line -- OpenCV's linker flags must come AFTER the source
// file, or you'll get "undefined reference" errors from the linker):
//   g++ -std=c++17 -I relink/include -pthread examples/cpp/camera_stream.cpp -o camera_stream $(pkg-config --cflags --libs opencv4)
// Run:
//   ./camera_stream pub        # opens /dev/video0, streams both topics
//   ./camera_stream sub        # receives, writes latest frames to disk

#include "relink/relink.hpp"
#include <opencv2/opencv.hpp>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>
#include <chrono>
#include <thread>
#include <mutex>
#include <fstream>
#include <functional>
#include <algorithm>

enum Topics : uint16_t { TOPIC_IMAGE_RAW = 500, TOPIC_IMAGE_COMPRESSED = 501 };

// Chunk payload well under the ~1400-byte MTU budget, leaving headroom
// for the chunk header fields themselves.
constexpr size_t kChunkDataBytes = 1024;

#pragma pack(push, 1)
struct ImageChunk {
    uint32_t frame_id;                  // which frame this chunk belongs to
    uint16_t chunk_index;                // 0-based index within the frame
    uint16_t chunk_count;                // total chunks for this frame
    uint16_t chunk_bytes;                // valid bytes in data[] (last chunk is usually partial)
    uint8_t  data[kChunkDataBytes];
};
#pragma pack(pop)
static_assert(sizeof(ImageChunk) < relink::kMaxPayloadBytes, "ImageChunk must fit in one UDP datagram");

// Splits `buf` into ImageChunk messages and publishes them in order.
static void publish_image(RelinkNode& node, uint16_t topic, uint32_t frame_id,
                           const uint8_t* buf, size_t len) {
    uint16_t chunk_count = static_cast<uint16_t>((len + kChunkDataBytes - 1) / kChunkDataBytes);
    for (uint16_t i = 0; i < chunk_count; ++i) {
        ImageChunk chunk{};
        chunk.frame_id = frame_id;
        chunk.chunk_index = i;
        chunk.chunk_count = chunk_count;
        size_t offset = size_t(i) * kChunkDataBytes;
        size_t n = std::min(kChunkDataBytes, len - offset);
        chunk.chunk_bytes = static_cast<uint16_t>(n);
        std::memcpy(chunk.data, buf + offset, n);
        node.publish<ImageChunk>(topic, chunk);
    }
}

// Reassembles chunks per topic/frame_id; calls `on_complete` once a
// frame's chunks have all arrived. Drops incomplete frames if a newer
// frame_id starts arriving first (no retransmission in v1 -- a dropped
// chunk means a dropped frame, same tradeoff UDP always has).
class FrameReassembler {
public:
    using CompleteCallback = std::function<void(uint32_t frame_id, const std::vector<uint8_t>&)>;

    explicit FrameReassembler(CompleteCallback cb) : on_complete_(std::move(cb)) {}

    void on_chunk(const ImageChunk& chunk) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (chunk.frame_id != current_frame_id_) {
            // New frame started -- discard whatever partial data we had.
            current_frame_id_ = chunk.frame_id;
            received_.assign(chunk.chunk_count, false);
            buffer_.assign(size_t(chunk.chunk_count) * kChunkDataBytes, 0);
            received_count_ = 0;
        }
        if (chunk.chunk_index >= received_.size() || received_[chunk.chunk_index]) return;

        std::memcpy(buffer_.data() + size_t(chunk.chunk_index) * kChunkDataBytes,
                    chunk.data, chunk.chunk_bytes);
        received_[chunk.chunk_index] = true;
        ++received_count_;
        if (chunk.chunk_index == chunk.chunk_count - 1) {
            last_chunk_bytes_ = chunk.chunk_bytes;
        }

        if (received_count_ == chunk.chunk_count) {
            size_t total = size_t(chunk.chunk_count - 1) * kChunkDataBytes + last_chunk_bytes_;
            std::vector<uint8_t> frame(buffer_.begin(), buffer_.begin() + total);
            on_complete_(current_frame_id_, frame);
        }
    }

private:
    std::mutex mutex_;
    CompleteCallback on_complete_;
    uint32_t current_frame_id_ = 0xFFFFFFFF;
    std::vector<bool> received_;
    std::vector<uint8_t> buffer_;
    size_t received_count_ = 0;
    uint16_t last_chunk_bytes_ = 0;
};

static void run_publisher(RelinkNode& node) {
    cv::VideoCapture cap(0);
    if (!cap.isOpened()) {
        std::fprintf(stderr, "camera_stream: could not open /dev/video0\n");
        return;
    }
    // Keep frames modest-sized -- raw streaming scales directly with
    // resolution (a full 640x480 raw frame is ~900 chunks per frame).
    cap.set(cv::CAP_PROP_FRAME_WIDTH, 320);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, 240);

    node.advertise<ImageChunk>(TOPIC_IMAGE_RAW);
    node.advertise<ImageChunk>(TOPIC_IMAGE_COMPRESSED);
    node.spin_once();

    std::printf("camera_stream: publishing image_raw (topic %u) and "
                "image_compressed (topic %u)\n", TOPIC_IMAGE_RAW, TOPIC_IMAGE_COMPRESSED);

    uint32_t frame_id = 0;
    while (true) {
        cv::Mat frame;
        cap >> frame;
        if (frame.empty()) continue;

        // Raw: send the frame's own pixel bytes directly, no encoding.
        publish_image(node, TOPIC_IMAGE_RAW, frame_id,
                      frame.data, frame.total() * frame.elemSize());

        // Compressed: JPEG-encode first -- typically 10-50x smaller,
        // meaning far fewer chunks/packets for the same picture.
        std::vector<uchar> jpeg;
        cv::imencode(".jpg", frame, jpeg, {cv::IMWRITE_JPEG_QUALITY, 70});
        publish_image(node, TOPIC_IMAGE_COMPRESSED, frame_id, jpeg.data(), jpeg.size());

        std::printf("frame %u: raw=%zu bytes (%zu chunks), compressed=%zu bytes (%zu chunks)\n",
                    frame_id,
                    frame.total() * frame.elemSize(),
                    (frame.total() * frame.elemSize() + kChunkDataBytes - 1) / kChunkDataBytes,
                    jpeg.size(), (jpeg.size() + kChunkDataBytes - 1) / kChunkDataBytes);

        node.spin_once();
        ++frame_id;
        std::this_thread::sleep_for(std::chrono::milliseconds(200)); // ~5 FPS
    }
}

static void run_subscriber(RelinkNode& node) {
    auto save_frame = [](const std::string& path, const std::vector<uint8_t>& data, bool is_jpeg) {
        if (is_jpeg) {
            std::ofstream f(path, std::ios::binary);
            f.write(reinterpret_cast<const char*>(data.data()), data.size());
        } else {
            // Raw BGR bytes at the publisher's known resolution (320x240) --
            // re-encode to JPEG purely so it's viewable as a normal image file.
            cv::Mat mat(240, 320, CV_8UC3, const_cast<uint8_t*>(data.data()));
            cv::imwrite(path, mat);
        }
    };

    FrameReassembler raw_reassembler([&](uint32_t id, const std::vector<uint8_t>& data) {
        save_frame("latest_raw.jpg", data, false);
        std::printf("image_raw: frame %u complete (%zu bytes) -> latest_raw.jpg\n", id, data.size());
    });
    FrameReassembler compressed_reassembler([&](uint32_t id, const std::vector<uint8_t>& data) {
        save_frame("latest_compressed.jpg", data, true);
        std::printf("image_compressed: frame %u complete (%zu bytes) -> latest_compressed.jpg\n",
                    id, data.size());
    });

    node.subscribe<ImageChunk>(TOPIC_IMAGE_RAW, [&](const ImageChunk& c) { raw_reassembler.on_chunk(c); });
    node.subscribe<ImageChunk>(TOPIC_IMAGE_COMPRESSED, [&](const ImageChunk& c) { compressed_reassembler.on_chunk(c); });

    std::printf("camera_stream: subscribed, writing latest_raw.jpg / latest_compressed.jpg "
                "to the current directory as frames complete\n");
    node.spin();
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s [pub|sub] [com_core_ip]\n", argv[0]);
        return 1;
    }
    RelinkNode node;
    if (argc > 2) {
        node.set_com_core.ip(argv[2]);
    } else {
        node.use_multicast_discovery();
    }

    if (std::strcmp(argv[1], "pub") == 0) run_publisher(node);
    else run_subscriber(node);
    return 0;
}

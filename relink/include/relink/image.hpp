// ReLink Image type -- a library-provided large-blob wire type that
// automatically chunks to the MTU maximum on send and reassembles on
// receive, promoted from the pattern shown in examples/cpp/camera_stream.cpp
// into a reusable feature.
//
// ReLink's core design deliberately does not fragment large messages
// for you ("one message, one UDP datagram" -- see frame.hpp's
// kMaxPayloadBytes and encode_frame's rejection of oversized payloads).
// Image is not an exception to that rule -- it's built entirely on top
// of the existing fixed-size publish<T>()/subscribe<T>() machinery, one
// ordinary trivially-copyable ImageChunk message per datagram, exactly
// the same as if you'd hand-written the chunking yourself. What this
// header adds is just the bookkeeping: computing the MTU-maximizing
// chunk size once, splitting/reassembling automatically, and matching
// per-frame chunks by frame_id so a caller gets one callback per
// complete image instead of per chunk.
//
// Despite the name, this is not JPEG/PNG-specific -- it carries whatever
// bytes you give it (raw pixels, a compressed buffer, any other large
// blob). See examples/cpp/camera_stream.cpp for it used both ways.

#pragma once

#include "relink/frame.hpp"
#include <cstdint>
#include <cstring>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <functional>
#include <algorithm>

namespace relink {

// Chunk data size is computed to fill the MTU budget as fully as
// possible: kMaxPayloadBytes minus this header's own fixed fields
// (frame_id + chunk_index + chunk_count + chunk_bytes = 10 bytes),
// rather than an arbitrary smaller constant -- every chunk datagram
// uses the full available payload space except (necessarily) the last,
// partial one.
inline constexpr size_t kImageChunkHeaderBytes = sizeof(uint32_t) + 3 * sizeof(uint16_t);
inline constexpr size_t kImageChunkDataBytes = kMaxPayloadBytes - kImageChunkHeaderBytes;

#pragma pack(push, 1)
struct ImageChunk {
    uint32_t frame_id;                       // which image this chunk belongs to
    uint16_t chunk_index;                     // 0-based index within the image
    uint16_t chunk_count;                     // total chunks for this image
    uint16_t chunk_bytes;                     // valid bytes in data[] (last chunk is usually partial)
    uint8_t  data[kImageChunkDataBytes];
};
#pragma pack(pop)
static_assert(sizeof(ImageChunk) <= kMaxPayloadBytes,
              "ImageChunk must fit in one UDP datagram -- this is a library invariant, not user-tunable");
static_assert(kImageChunkDataBytes > 0, "MTU budget too small for any image chunk payload");

// Maximum image size representable: chunk_count is a uint16_t.
inline constexpr size_t kMaxImageBytes = size_t(UINT16_MAX) * kImageChunkDataBytes;

enum class ImageEncodeResult { Ok, TooLarge };

// Splits `data` into ImageChunk messages sized to the MTU maximum and
// invokes `send_chunk` once per chunk, in order. Kept transport-agnostic
// (a callback rather than calling publish<T>() directly) so this header
// has no dependency on RelinkNode/UdpTransport -- RelinkNode's
// publish_image() wraps this with `send_chunk` bound to its own
// publish<ImageChunk>() call.
template <typename SendChunkFn>
ImageEncodeResult encode_image_chunks(uint32_t frame_id, const uint8_t* data, size_t len,
                                       SendChunkFn&& send_chunk) {
    if (len > kMaxImageBytes) return ImageEncodeResult::TooLarge;

    uint16_t chunk_count = static_cast<uint16_t>(
        (len + kImageChunkDataBytes - 1) / kImageChunkDataBytes);
    if (chunk_count == 0) chunk_count = 1; // a zero-length image is still one (empty) chunk

    for (uint16_t i = 0; i < chunk_count; ++i) {
        ImageChunk chunk{};
        chunk.frame_id = frame_id;
        chunk.chunk_index = i;
        chunk.chunk_count = chunk_count;
        size_t offset = size_t(i) * kImageChunkDataBytes;
        size_t n = std::min(kImageChunkDataBytes, len - offset);
        chunk.chunk_bytes = static_cast<uint16_t>(n);
        if (n > 0) std::memcpy(chunk.data, data + offset, n);
        send_chunk(chunk);
    }
    return ImageEncodeResult::Ok;
}

// Reassembles ImageChunk messages for ONE topic; calls on_complete once
// a frame's chunks have all arrived. If a new frame_id starts arriving
// before the previous one completed, the previous (incomplete) frame is
// discarded -- no retransmission, no partial delivery, the same
// tradeoff UDP always has (documented in camera_stream.cpp's real
// measured results: this is why compressing before sending matters).
class ImageReassembler {
public:
    using CompleteCallback = std::function<void(uint32_t frame_id, const std::vector<uint8_t>&)>;

    explicit ImageReassembler(CompleteCallback cb) : on_complete_(std::move(cb)) {}

    void on_chunk(const ImageChunk& chunk) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (chunk.frame_id != current_frame_id_ || !in_progress_) {
            current_frame_id_ = chunk.frame_id;
            in_progress_ = true;
            received_.assign(chunk.chunk_count, false);
            buffer_.assign(size_t(chunk.chunk_count) * kImageChunkDataBytes, 0);
            received_count_ = 0;
        }
        if (chunk.chunk_index >= received_.size() || received_[chunk.chunk_index]) return;

        std::memcpy(buffer_.data() + size_t(chunk.chunk_index) * kImageChunkDataBytes,
                    chunk.data, chunk.chunk_bytes);
        received_[chunk.chunk_index] = true;
        ++received_count_;
        if (chunk.chunk_index == chunk.chunk_count - 1) {
            last_chunk_bytes_ = chunk.chunk_bytes;
        }

        if (received_count_ == chunk.chunk_count) {
            size_t total = (chunk.chunk_count > 1)
                ? size_t(chunk.chunk_count - 1) * kImageChunkDataBytes + last_chunk_bytes_
                : last_chunk_bytes_;
            std::vector<uint8_t> image(buffer_.begin(), buffer_.begin() + total);
            in_progress_ = false;
            on_complete_(current_frame_id_, image);
        }
    }

private:
    std::mutex mutex_;
    CompleteCallback on_complete_;
    bool in_progress_ = false;
    uint32_t current_frame_id_ = 0;
    std::vector<bool> received_;
    std::vector<uint8_t> buffer_;
    size_t received_count_ = 0;
    uint16_t last_chunk_bytes_ = 0;
};

} // namespace relink

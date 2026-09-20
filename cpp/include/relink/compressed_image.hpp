// ReLink CompressedImage type -- like image.hpp's Image, but for
// pre-compressed image/video frames (JPEG, etc.) that also carry two
// extra pieces of metadata end-to-end, with no side channel back to
// the publisher: the producer's capture timestamp (for latency
// measurement) and the encoder quality actually used for this frame
// (informational, e.g. for overlaying "Q=42" on a debug view). See
// adaptive_bitrate.hpp for the quality-selection logic this type is
// meant to pair with, and examples/cpp/camera_stream.cpp for it used
// end to end.
//
// Same wire philosophy as Image: one ordinary chunk message per
// datagram, MTU-chunked and reassembled automatically, no
// retransmission -- an image whose chunks don't all arrive before the
// next frame starts is silently dropped. The two extra fields are
// repeated on EVERY chunk (not just chunk 0) because UDP does not
// guarantee delivery order -- a receiver must be able to recover them
// even if the first chunk to arrive isn't chunk_index 0.

#pragma once

#include "relink/frame.hpp"
#include <cstdint>
#include <cstring>
#include <vector>
#include <mutex>
#include <functional>
#include <algorithm>

namespace relink {

// frame_id(4) + chunk_index(2) + chunk_count(2) + chunk_bytes(2) +
// capture_timestamp_ns(8) + quality(1) = 19 bytes.
inline constexpr size_t kCompressedImageChunkHeaderBytes =
    sizeof(uint32_t) + 3 * sizeof(uint16_t) + sizeof(uint64_t) + sizeof(uint8_t);
inline constexpr size_t kCompressedImageChunkDataBytes =
    kMaxPayloadBytes - kCompressedImageChunkHeaderBytes;

#pragma pack(push, 1)
struct CompressedImageChunk {
    uint32_t frame_id;                                    // which image this chunk belongs to
    uint16_t chunk_index;                                  // 0-based index within the image
    uint16_t chunk_count;                                  // total chunks for this image
    uint16_t chunk_bytes;                                  // valid bytes in data[]
    uint64_t capture_timestamp_ns;                         // producer's capture time
    uint8_t  quality;                                      // encoder quality used, 0-100 (0 = unknown/n-a)
    uint8_t  data[kCompressedImageChunkDataBytes];
};
#pragma pack(pop)
static_assert(sizeof(CompressedImageChunk) <= kMaxPayloadBytes,
              "CompressedImageChunk must fit in one UDP datagram -- a library invariant, not user-tunable");
static_assert(kCompressedImageChunkDataBytes > 0, "MTU budget too small for any chunk payload");

// Maximum image size representable: chunk_count is a uint16_t.
inline constexpr size_t kMaxCompressedImageBytes = size_t(UINT16_MAX) * kCompressedImageChunkDataBytes;

enum class CompressedImageEncodeResult { Ok, TooLarge };

// Splits `data` into CompressedImageChunk messages sized to the MTU
// maximum and invokes `send_chunk` once per chunk, in order.
// `capture_timestamp_ns` and `quality` are stamped onto every chunk.
template <typename SendChunkFn>
CompressedImageEncodeResult encode_compressed_image_chunks(
    uint32_t frame_id, const uint8_t* data, size_t len,
    uint64_t capture_timestamp_ns, uint8_t quality,
    SendChunkFn&& send_chunk) {
    if (len > kMaxCompressedImageBytes) return CompressedImageEncodeResult::TooLarge;

    uint16_t chunk_count = static_cast<uint16_t>(
        (len + kCompressedImageChunkDataBytes - 1) / kCompressedImageChunkDataBytes);
    if (chunk_count == 0) chunk_count = 1; // a zero-length image is still one (empty) chunk

    for (uint16_t i = 0; i < chunk_count; ++i) {
        CompressedImageChunk chunk{};
        chunk.frame_id = frame_id;
        chunk.chunk_index = i;
        chunk.chunk_count = chunk_count;
        chunk.capture_timestamp_ns = capture_timestamp_ns;
        chunk.quality = quality;
        size_t offset = size_t(i) * kCompressedImageChunkDataBytes;
        size_t n = std::min(kCompressedImageChunkDataBytes, len - offset);
        chunk.chunk_bytes = static_cast<uint16_t>(n);
        if (n > 0) std::memcpy(chunk.data, data + offset, n);
        send_chunk(chunk);
    }
    return CompressedImageEncodeResult::Ok;
}

// Reassembles CompressedImageChunk messages for ONE topic; calls
// on_complete(frame_id, data, capture_timestamp_ns, quality) once a
// frame's chunks have all arrived. Same discard-on-new-frame_id
// tradeoff as ImageReassembler.
class CompressedImageReassembler {
public:
    using CompleteCallback = std::function<void(uint32_t frame_id, const std::vector<uint8_t>& data,
                                                 uint64_t capture_timestamp_ns, uint8_t quality)>;

    explicit CompressedImageReassembler(CompleteCallback cb) : on_complete_(std::move(cb)) {}

    void on_chunk(const CompressedImageChunk& chunk) {
        on_chunk_raw(chunk.frame_id, chunk.chunk_index, chunk.chunk_count,
                     chunk.data, chunk.chunk_bytes, chunk.capture_timestamp_ns, chunk.quality);
    }

    // Same reassembly logic as on_chunk(), but takes the chunk's
    // fields and a data pointer directly -- used by RelinkNode's
    // subscribe_compressed_image() zero-copy receive path, mirroring
    // image.hpp's on_chunk_raw().
    void on_chunk_raw(uint32_t frame_id, uint16_t chunk_index, uint16_t chunk_count,
                       const uint8_t* data, uint16_t chunk_bytes,
                       uint64_t capture_timestamp_ns, uint8_t quality) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (frame_id != current_frame_id_ || !in_progress_) {
            current_frame_id_ = frame_id;
            in_progress_ = true;
            received_.assign(chunk_count, false);
            buffer_.assign(size_t(chunk_count) * kCompressedImageChunkDataBytes, 0);
            received_count_ = 0;
            capture_timestamp_ns_ = capture_timestamp_ns;
            quality_ = quality;
        }
        if (chunk_index >= received_.size() || received_[chunk_index]) return;

        std::memcpy(buffer_.data() + size_t(chunk_index) * kCompressedImageChunkDataBytes,
                    data, chunk_bytes);
        received_[chunk_index] = true;
        ++received_count_;
        if (chunk_index == chunk_count - 1) {
            last_chunk_bytes_ = chunk_bytes;
        }

        if (received_count_ == chunk_count) {
            size_t total = (chunk_count > 1)
                ? size_t(chunk_count - 1) * kCompressedImageChunkDataBytes + last_chunk_bytes_
                : last_chunk_bytes_;
            std::vector<uint8_t> image(buffer_.begin(), buffer_.begin() + total);
            in_progress_ = false;
            on_complete_(current_frame_id_, image, capture_timestamp_ns_, quality_);
        }
    }

private:
    std::mutex mutex_;
    CompleteCallback on_complete_;
    bool in_progress_ = false;
    uint32_t current_frame_id_ = 0;
    uint64_t capture_timestamp_ns_ = 0;
    uint8_t quality_ = 0;
    std::vector<bool> received_;
    std::vector<uint8_t> buffer_;
    size_t received_count_ = 0;
    uint16_t last_chunk_bytes_ = 0;
};

} // namespace relink

// Step 2: fixed-size ring buffer per topic (per relink-com-spec.md
// "Buffer" section).
//
// - Pre-allocated, fixed capacity, no dynamic resizing/growth.
// - On overflow (push while full), drop the OLDEST element and insert
//   the new one — the publisher/receiver never blocks and never fails
//   on a full buffer.
// - No heap allocation on push/pop (storage is a plain array member) —
//   required by the 1000Hz hot-path budget in the spec.
// - Single-producer/single-consumer usage is the intended v1 case (one
//   data thread pushes on receive, the same thread's callback pops), so
//   this is not made thread-safe here; a lock-free SPSC design is called
//   out as a later Lean-optimization step, not part of the basic v1
//   buffer built in this step.

#pragma once

#include <cstddef>
#include <array>
#include <optional>
#include <type_traits>

namespace relink {

template <typename T, size_t Capacity>
class RingBuffer {
    static_assert(Capacity > 0, "RingBuffer capacity must be > 0");
    static_assert(std::is_trivially_copyable<T>::value,
                  "RingBuffer<T> requires a trivially copyable T (wire types only)");

public:
    RingBuffer() = default;

    // Push a new element. If the buffer is full, the oldest element is
    // dropped to make room — this call never blocks and never fails.
    // Returns true if an existing element was dropped to make room,
    // false if the push just filled previously-free space (useful for
    // overflow-counting/telemetry without adding a second code path).
    bool push(const T& value) {
        bool dropped = false;
        if (count_ == Capacity) {
            // Buffer full: advance head to discard the oldest slot first.
            head_ = (head_ + 1) % Capacity;
            dropped = true;
        } else {
            ++count_;
        }
        data_[tail_] = value;
        tail_ = (tail_ + 1) % Capacity;
        return dropped;
    }

    // Pop the oldest element. Returns std::nullopt if empty.
    std::optional<T> pop() {
        if (count_ == 0) return std::nullopt;
        T value = data_[head_];
        head_ = (head_ + 1) % Capacity;
        --count_;
        return value;
    }

    size_t size() const { return count_; }
    constexpr size_t capacity() const { return Capacity; }
    bool empty() const { return count_ == 0; }
    bool full() const { return count_ == Capacity; }

    void clear() {
        head_ = 0;
        tail_ = 0;
        count_ = 0;
    }

private:
    std::array<T, Capacity> data_{};
    size_t head_ = 0;  // index of oldest element
    size_t tail_ = 0;  // index where next element is written
    size_t count_ = 0;
};

} // namespace relink

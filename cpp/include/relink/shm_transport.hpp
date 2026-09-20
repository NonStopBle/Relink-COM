// Same-host IPC transport: a lock-free SPSC shared-memory ring buffer,
// used by RelinkNode's advertise_local_ipc/subscribe_local_ipc/
// publish_local_ipc as an explicit opt-in alternative to UDP for two
// processes known to be on the same machine. Mirrors
// relink/shm_transport.py byte-for-byte -- a C++ and Python process
// can share one ring.
//
// max_payload is a runtime parameter (default kShmMaxPayload, matching
// UDP's MTU-driven MAX_PAYLOAD_BYTES for small messages), NOT a fixed
// ceiling -- both sides just have to agree on the same value for a
// given topic, same as they already have to agree on capacity. This is
// what lets Image/CompressedImage frames go over local IPC as ONE
// slot, whole, with no chunking: shared memory has no MTU, so the only
// reason those types chunk over UDP (the 1400-byte payload cap) simply
// doesn't apply here. advertise_local_ipc_image()-style callers in
// relink.hpp just pass a larger max_payload; the ring code below is
// otherwise identical for small messages and image frames.
//
// Layout: [ShmRingHeader][slot 0][slot 1]...[slot capacity-1]
// Each slot: uint32 seq_num, uint32 payload_len, uint8 payload[max_payload]
//
// SPSC discipline: only the producer writes `tail`, only the consumer
// writes `head`. Each side only ever READS the other's index. Aligned
// uint32 reads/writes are atomic at the hardware level on x86-64/
// ARM64, so this needs no OS-level lock -- std::atomic here is just to
// stop the compiler from reordering/caching the load across the mmap
// boundary, not for hardware atomicity.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <chrono>
#include <thread>
#include <functional>

namespace relink {

constexpr uint32_t kShmRingMagic = 0x524C4B31;  // "RLK1"
constexpr uint32_t kShmRingVersion = 1;
constexpr uint32_t kShmMaxPayload = 1400;  // default, matches MAX_PAYLOAD_BYTES
constexpr uint32_t kShmDefaultCapacity = 1024;

struct ShmRingHeader {
    std::atomic<uint32_t> magic;     // 0 until creator finishes init; also
                                      // doubles as the stale-takeover claim
                                      // gate -- see open()'s EEXIST branch.
    uint32_t version;
    uint32_t slot_size;
    uint32_t capacity;
    std::atomic<uint32_t> head;      // consumer-owned: index of next slot to read
    std::atomic<uint32_t> tail;      // producer-owned: index of next slot to write
    uint32_t creator_pid;            // liveness check for stale-segment detection
};

// One ring per (topic, publisher-process, subscriber-process) pair --
// strictly single-producer/single-consumer, same fan-out model as
// RelinkNode's existing per-peer UDP unicast.
class ShmRing {
public:
    ~ShmRing() { close_mapping(); }

    // Opens (creating if necessary) a named ring. Either side may call
    // this first -- whichever wins the O_CREAT|O_EXCL race becomes the
    // creator and initializes the header; the other attaches and polls
    // briefly for magic to become valid.
    bool open(const std::string& name, uint32_t capacity = kShmDefaultCapacity,
              uint32_t max_payload = kShmMaxPayload) {
        name_ = name;
        max_payload_ = max_payload;
        slot_size_ = 8 + max_payload;  // seq_num(4) + payload_len(4) + payload
        size_t map_size = sizeof(ShmRingHeader) + static_cast<size_t>(capacity) * slot_size_;

        int fd = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0666);
        if (fd >= 0) {
            creator_ = true;
            if (ftruncate(fd, map_size) != 0) { ::close(fd); return false; }
        } else if (errno == EEXIST) {
            fd = shm_open(name.c_str(), O_RDWR, 0666);
            if (fd < 0) return false;
            creator_ = false;
        } else {
            return false;
        }

        void* base = mmap(nullptr, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ::close(fd);  // fd not needed after mmap
        if (base == MAP_FAILED) return false;

        base_ = base;
        map_size_ = map_size;
        hdr_ = reinterpret_cast<ShmRingHeader*>(base_);
        slots_ = reinterpret_cast<uint8_t*>(base_) + sizeof(ShmRingHeader);

        if (creator_) {
            init_as_creator(capacity);
            return true;
        }

        // Attacher: the segment may be (a) mid-init by a live creator --
        // poll briefly, or (b) leftover from a creator that crashed
        // without unlinking -- detect via a dead creator_pid and steal
        // it by winning a CAS on magic, rather than blindly attaching to
        // whatever head/tail garbage a crash left behind.
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (true) {
            uint32_t m = hdr_->magic.load(std::memory_order_acquire);
            if (m == kShmRingMagic) {
                if (hdr_->version != kShmRingVersion || hdr_->slot_size != slot_size_
                    || hdr_->capacity != capacity) {
                    return false;  // incompatible segment, not just stale
                }
                pid_t owner = static_cast<pid_t>(hdr_->creator_pid);
                if (owner != getpid() && kill(owner, 0) != 0 && errno == ESRCH) {
                    // Recorded owner is dead -- try to steal by CASing
                    // magic to 0 first. Whoever wins becomes the new
                    // creator; losers just fall through to re-poll.
                    uint32_t expected = kShmRingMagic;
                    if (hdr_->magic.compare_exchange_strong(expected, 0, std::memory_order_acq_rel)) {
                        creator_ = true;
                        init_as_creator(capacity);
                        return true;
                    }
                    continue;  // lost the race -- another process is reinitializing now
                }
                return true;  // owner is alive (or is us) -- normal attach
            }
            if (std::chrono::steady_clock::now() > deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    // Producer side. Never blocks -- returns false if the ring is full,
    // explicit backpressure rather than silent loss.
    bool try_push(const uint8_t* payload, uint32_t len, uint32_t seq_num) {
        if (len > max_payload_) return false;
        uint32_t tail = hdr_->tail.load(std::memory_order_relaxed);
        uint32_t head = hdr_->head.load(std::memory_order_acquire);
        uint32_t next = (tail + 1) % hdr_->capacity;
        if (next == head) return false;  // full

        uint8_t* slot = slots_ + static_cast<size_t>(tail) * slot_size_;
        std::memcpy(slot, &seq_num, 4);
        std::memcpy(slot + 4, &len, 4);
        std::memcpy(slot + 8, payload, len);
        hdr_->tail.store(next, std::memory_order_release);
        return true;
    }

    // Consumer side. Never blocks -- returns false if the ring is empty.
    bool try_pop(uint8_t* out_payload, uint32_t& out_len, uint32_t& out_seq_num) {
        uint32_t head = hdr_->head.load(std::memory_order_relaxed);
        uint32_t tail = hdr_->tail.load(std::memory_order_acquire);
        if (head == tail) return false;  // empty

        uint8_t* slot = slots_ + static_cast<size_t>(head) * slot_size_;
        std::memcpy(&out_seq_num, slot, 4);
        std::memcpy(&out_len, slot + 4, 4);
        std::memcpy(out_payload, slot + 8, out_len);
        hdr_->head.store((head + 1) % hdr_->capacity, std::memory_order_release);
        return true;
    }

    // Zero-copy consumer variant: hands `fn` a pointer DIRECTLY into the
    // shared-memory slot, with no memcpy -- matching the same "valid
    // only during the callback, don't retain it" contract
    // UdpTransport's raw callback already uses for its reused receive
    // buffer (see udp_transport.hpp's recv_and_dispatch()), not a new
    // risk pattern for this codebase. `head` only advances AFTER fn
    // returns, so the slot can't be overwritten by the producer while
    // fn is still reading it. Worth it specifically for large payloads
    // (Image/CompressedImage frames) where try_pop()'s extra memcpy
    // would otherwise undo a real fraction of the point of using shared
    // memory in the first place; for small messages the copy is cheap
    // enough that try_pop() above is simpler and just as fine.
    bool try_pop_zero_copy(const std::function<void(const uint8_t* payload, uint32_t len, uint32_t seq_num)>& fn) {
        uint32_t head = hdr_->head.load(std::memory_order_relaxed);
        uint32_t tail = hdr_->tail.load(std::memory_order_acquire);
        if (head == tail) return false;  // empty

        uint8_t* slot = slots_ + static_cast<size_t>(head) * slot_size_;
        uint32_t seq_num, len;
        std::memcpy(&seq_num, slot, 4);
        std::memcpy(&len, slot + 4, 4);
        fn(slot + 8, len, seq_num);
        hdr_->head.store((head + 1) % hdr_->capacity, std::memory_order_release);
        return true;
    }

    bool is_creator() const { return creator_; }
    uint32_t max_payload() const { return max_payload_; }

    // Creator calls this on clean shutdown to remove the segment from
    // /dev/shm. A crash skips this -- the dead-owner detection in
    // open() is what recovers a leaked segment on the next run.
    void unlink() {
        if (!name_.empty()) shm_unlink(name_.c_str());
    }

    void close_mapping() {
        if (base_) { munmap(base_, map_size_); base_ = nullptr; }
    }

private:
    void init_as_creator(uint32_t capacity) {
        hdr_->version = kShmRingVersion;
        hdr_->slot_size = slot_size_;
        hdr_->capacity = capacity;
        hdr_->head.store(0, std::memory_order_relaxed);
        hdr_->tail.store(0, std::memory_order_relaxed);
        hdr_->creator_pid = static_cast<uint32_t>(getpid());
        hdr_->magic.store(kShmRingMagic, std::memory_order_release);  // publish last
    }

    void* base_ = nullptr;
    size_t map_size_ = 0;
    ShmRingHeader* hdr_ = nullptr;
    uint8_t* slots_ = nullptr;
    std::string name_;
    bool creator_ = false;
    uint32_t max_payload_ = kShmMaxPayload;
    uint32_t slot_size_ = kShmMaxPayload + 8;
};

}  // namespace relink

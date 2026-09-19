// Step 3 (part 2): raw UDP send/receive core, running on its own
// dedicated data thread, per relink-com-spec.md's "Threading and core
// allocation" section. This step builds only the data path
// (secure=false, checksum=false); discovery (rlcore/multicast) is
// steps 4-5, and the templated advertise/subscribe/publish<T> API is
// step 6. Here the "topic" concept is just a numeric ID with a raw
// byte-payload callback -- typed dispatch is layered on top later.
//
// Hot-path rules enforced by this implementation (per spec):
// - No heap allocation in send/receive: buffers are fixed-size members,
//   reused every call.
// - No blocking-forever recv: recvfrom uses a bounded socket timeout so
//   the loop can be stopped promptly and never stalls indefinitely.
// - Callback runs inline on the data thread (per spec's "keep subscriber
//   callbacks on the data thread" v1 decision) -- callers must keep
//   callbacks fast.

#pragma once

#include "relink/frame.hpp"
#include <atomic>
#include <thread>
#include <functional>
#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>

#include <sys/socket.h>
#include <sys/uio.h>
#include <sched.h>
#include <pthread.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

namespace relink {

struct PeerAddr {
    uint32_t ip_host_order; // host byte order, e.g. from inet_addr + ntohl
    uint16_t port;
};

using RawTopicCallback = std::function<void(const uint8_t* payload, size_t payload_len)>;

class UdpTransport {
public:
    UdpTransport() = default;
    ~UdpTransport() { stop(); close_socket(); }

    UdpTransport(const UdpTransport&) = delete;
    UdpTransport& operator=(const UdpTransport&) = delete;

    // Binds a UDP socket on `bind_port` (0 lets the OS pick an ephemeral
    // port). Throws std::runtime_error on failure -- this is a one-time
    // setup call, not on the hot path.
    void bind(uint16_t bind_port) {
        sock_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (sock_ < 0) {
            throw std::runtime_error("UdpTransport: socket() failed");
        }

        // Bounded recv timeout so the data-thread loop can check the
        // stop flag periodically instead of blocking forever -- this is
        // the "non-blocking or tight-timeout recv loop" rule from the
        // spec's Performance target section.
        struct timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 50 * 1000; // 50ms poll interval for stop responsiveness
        ::setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        // Only enlarge the buffer if this node actually uses Image (set
        // via enable_large_buffers(), called by advertise<ImageChunk>/
        // subscribe<ImageChunk> before this bind() runs) -- a plain
        // small-message node (the common, latency-sensitive case this
        // library is built for, measured ~3x faster than ROS2 Humble)
        // keeps the OS default buffer untouched. A bigger buffer is not
        // free: it's more memory reserved per socket and, under a
        // sustained overload, more queued backlog before a drop finally
        // happens (i.e. worse latency, not better) -- so it's only
        // requested for the one message type (several-hundred-chunk
        // Image bursts) that has been measured to actually need it. See
        // enable_large_buffers() below for the sizing rationale.
        if (want_large_buffers_) apply_large_buffers();

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(bind_port);

        if (::bind(sock_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            ::close(sock_);
            sock_ = -1;
            throw std::runtime_error("UdpTransport: bind() failed");
        }

        socklen_t len = sizeof(addr);
        if (::getsockname(sock_, reinterpret_cast<struct sockaddr*>(&addr), &len) == 0) {
            local_port_ = ntohs(addr.sin_port);
        }
    }

    uint16_t local_port() const { return local_port_; }

    // Opts this node's socket into a larger send/receive buffer, sized
    // to the lowest value that measured reliably for a multi-hundred-
    // chunk Image burst (not maxed out at an arbitrary large number): a
    // 20-frame, 5 FPS, 640x480 raw burst (664 chunks/frame, ~930KB/
    // frame) delivered 20/20 at 768KB-1MB repeatedly, but dropped 1/20
    // at 256-512KB. Call before bind() (advertise<ImageChunk>/
    // subscribe<ImageChunk> do this automatically); safe to call after
    // bind() too, applying immediately. A bigger buffer only masks
    // drops for a subscriber that keeps up -- a genuinely slow callback
    // still accumulates latency and eventually drops regardless of
    // buffer size (see subscribe_image's docs), so this is deliberately
    // NOT applied to every node by default, only ones that opt in by
    // using Image.
    void enable_large_buffers() {
        want_large_buffers_ = true;
        if (sock_ >= 0) apply_large_buffers();
    }

    // Exposes the underlying socket fd, needed ONLY so callers (namely
    // RelinkNode's rlcore registration step) can reuse this exact
    // socket for a pre-start() synchronous request/reply, keeping the
    // NAT-mapped source port rlcore observes consistent with the port
    // this transport will actually receive data on. Safe to use before
    // start() launches the dedicated data thread; not meant for general
    // use once the thread is running (it owns recv from that point on).
    int native_handle() const { return sock_; }

    // Register the raw-bytes handler for a topic_id. Must be called
    // before start() for the topics this node will receive traffic on
    // (v1: no locking needed here since registration happens before the
    // data thread starts, consistent with the spec's "type fixed per
    // topic at registration" design).
    void set_topic_handler(uint32_t topic_id, RawTopicCallback cb) {
        std::lock_guard<std::mutex> lock(handlers_mutex_);
        handlers_[topic_id] = std::move(cb);
    }

    // Opts this transport into dropping an exact-duplicate seq_num per
    // topic before it reaches the subscriber callback. Off by default
    // (extra map lookup on every receive) -- only relay fallback (see
    // relay_wire.hpp) turns this on, since that's the one case where the
    // SAME message can legitimately arrive twice on the same socket
    // (once direct, once relayed). A message's seq_num is assigned once
    // by the sender's monotonic per-transport counter and never reused
    // for a different payload, so "same topic, same seq_num as the last
    // one delivered" is a safe duplicate signal without needing to
    // inspect payload bytes.
    void enable_relay_dedup() { relay_dedup_enabled_ = true; }

    // Send one datagram to `peer`. Returns false (and does not send) if
    // payload_len exceeds the MTU budget -- reject loudly, never
    // silently truncate/fragment, per spec.
    bool publish_raw(uint32_t topic_id, const void* payload, size_t payload_len,
                      const PeerAddr& peer) {
        return publish_raw(topic_id, payload, payload_len, peer,
                            seq_counter_.fetch_add(1, std::memory_order_relaxed));
    }

    // Draws a fresh seq_num without sending -- so a caller that will
    // send the SAME logical message to several destinations (e.g. every
    // direct peer, plus a relay fallback copy) can give them all the
    // same seq_num via the overload below. That's what lets a
    // relay-dedup-enabled receiver recognize "direct and relayed copies
    // of one publish() call" as the same message rather than two.
    uint16_t next_seq() { return seq_counter_.fetch_add(1, std::memory_order_relaxed); }

    // Same as publish_raw() above but with an explicit seq_num instead
    // of drawing a new one -- see next_seq().
    bool publish_raw(uint32_t topic_id, const void* payload, size_t payload_len,
                      const PeerAddr& peer, uint16_t seq) {
        size_t frame_len = 0;
        EncodeResult r = encode_frame(topic_id, seq, payload, payload_len,
                                       send_buf_, sizeof(send_buf_), &frame_len);
        if (r != EncodeResult::Ok) {
            return false;
        }

        struct sockaddr_in dest{};
        dest.sin_family = AF_INET;
        dest.sin_addr.s_addr = htonl(peer.ip_host_order);
        dest.sin_port = htons(peer.port);

        ssize_t sent = ::sendto(sock_, send_buf_, frame_len, 0,
                                 reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest));
        return sent == static_cast<ssize_t>(frame_len);
    }

    // Zero-copy variant for large-blob types like Image: publish_raw()
    // above always memcpy's the whole payload into send_buf_ before
    // sendto(), which for Image means copying the caller's chunk data
    // twice (once into an ImageChunk struct, once into send_buf_) for
    // every one of a several-hundred-chunk burst. This instead uses
    // sendmsg() with a scatter-gather iovec list so `extra_header` (a
    // small fixed struct, e.g. Image's 10-byte chunk header) and `data`
    // (the caller's actual buffer, e.g. a camera frame slice) are handed
    // to the kernel directly -- no userspace copy of either, only the
    // 9 fixed framing bytes ('#' + 7-byte header + '\n') are ever
    // assembled locally.
    bool publish_scattered(uint32_t topic_id, const void* extra_header, size_t extra_header_len,
                            const void* data, size_t data_len, const PeerAddr& peer) {
        size_t payload_len = extra_header_len + data_len;
        if (payload_len > kMaxPayloadBytes) return false;

        uint16_t seq = seq_counter_.fetch_add(1, std::memory_order_relaxed);
        RelinkHeader header{};
        header.topic_id = topic_id;
        header.seq_num = seq;
        header.payload_len = static_cast<uint16_t>(payload_len);
        header.flags = 0;

        uint8_t start = kStartByte;
        uint8_t stop = kStopByte;

        struct iovec iov[5];
        int n = 0;
        iov[n].iov_base = &start; iov[n].iov_len = 1; ++n;
        iov[n].iov_base = &header; iov[n].iov_len = sizeof(header); ++n;
        if (extra_header_len > 0) {
            iov[n].iov_base = const_cast<void*>(extra_header); iov[n].iov_len = extra_header_len; ++n;
        }
        if (data_len > 0) {
            iov[n].iov_base = const_cast<void*>(data); iov[n].iov_len = data_len; ++n;
        }
        iov[n].iov_base = &stop; iov[n].iov_len = 1; ++n;

        struct sockaddr_in dest{};
        dest.sin_family = AF_INET;
        dest.sin_addr.s_addr = htonl(peer.ip_host_order);
        dest.sin_port = htons(peer.port);

        struct msghdr msg{};
        msg.msg_name = &dest;
        msg.msg_namelen = sizeof(dest);
        msg.msg_iov = iov;
        msg.msg_iovlen = n;

        size_t frame_len = 1 + sizeof(header) + payload_len + 1;
        ssize_t sent = ::sendmsg(sock_, &msg, 0);
        return sent == static_cast<ssize_t>(frame_len);
    }

    // Starts the dedicated data thread (recv loop + dispatch). Per spec,
    // this thread must never share responsibilities with discovery.
    //
    // pin_to_core: optional CPU core to pin the data thread to
    // (sched_setaffinity), per spec's Lean optimization / Threading
    // sections -- "the single highest-leverage fix for tail-latency
    // spikes (p99/worst-case)". -1 (default) leaves the thread
    // unpinned. Applied once, right after the thread starts.
    //
    // use_realtime: apply SCHED_FIFO to the data thread (spec: "consider
    // a real-time scheduling class... with care"). Per spec this is
    // per-thread, applied ONLY to the data thread, never process-wide.
    // If the process lacks CAP_SYS_NICE/RLIMIT_RTPRIO (the common case
    // when not run as root), this fails gracefully -- logs a warning and
    // continues at the normal scheduling policy, per the "apply with
    // care" / never-crash rule; it is not treated as fatal.
    void start(int pin_to_core = -1, bool use_realtime = false, int rt_priority = 10) {
        if (running_.exchange(true)) return; // already running
        pin_to_core_ = pin_to_core;
        use_realtime_ = use_realtime;
        rt_priority_ = rt_priority;
        thread_ = std::thread([this] { run_loop(); });
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (thread_.joinable()) thread_.join();
    }

    // Service one recv attempt without a dedicated thread (useful for
    // tests / a single-threaded caller-driven mode). Returns true if a
    // valid frame was received and dispatched.
    bool poll_once() {
        return recv_and_dispatch();
    }

    bool is_running() const { return running_.load(std::memory_order_relaxed); }

    // Blocks for up to `timeout_ms` for a non-frame packet (a
    // RegisterAck) that arrived on this socket after start(), handed off
    // by recv_and_dispatch() instead of raced for directly by a second
    // caller of recvfrom() on the same fd. See register_with_rlcore_on_socket()
    // in rlcore_client.hpp, the one caller of this. Returns true and
    // fills `out` if a packet arrived in time, false on timeout.
    bool get_register_reply(std::vector<uint8_t>* out, int timeout_ms) {
        std::unique_lock<std::mutex> lock(register_reply_mutex_);
        if (!register_reply_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                          [this] { return !register_reply_queue_.empty(); })) {
            return false;
        }
        *out = std::move(register_reply_queue_.front());
        register_reply_queue_.pop();
        return true;
    }

private:
    void apply_large_buffers() {
        int bufsize = 1024 * 1024;
        ::setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
        ::setsockopt(sock_, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
    }

    void close_socket() {
        if (sock_ >= 0) {
            ::close(sock_);
            sock_ = -1;
        }
    }

    void run_loop() {
        if (pin_to_core_ >= 0) {
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(pin_to_core_, &cpuset);
            ::pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
        }
        if (use_realtime_) {
            struct sched_param param{};
            param.sched_priority = rt_priority_;
            int rc = ::pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
            if (rc != 0) {
                std::fprintf(stderr,
                    "UdpTransport: SCHED_FIFO request failed (%s) -- continuing at normal "
                    "scheduling priority. Run with CAP_SYS_NICE or as root to enable it.\n",
                    std::strerror(rc));
            }
        }
        while (running_.load(std::memory_order_relaxed)) {
            recv_and_dispatch();
        }
    }

    bool recv_and_dispatch() {
        struct sockaddr_in src{};
        socklen_t src_len = sizeof(src);
        // 65507 (largest possible UDP/IPv4 datagram), not kMaxFrameBytes
        // (~1411, sized for the data-frame payload cap of 1400 bytes):
        // this socket also carries RegisterAck replies (see
        // register_reply_queue_ below), which have no relation to that
        // cap -- a rlcore reply listing many peers can be several KB.
        // recvfrom()'s length argument is a hard truncation point for
        // UDP (the kernel discards anything past it, silently), so a
        // too-small buffer here doesn't just clip a frame, it corrupts a
        // legitimate large RegisterAck into something
        // decode_register_ack() rejects -- which looks exactly like
        // "rlcore never replied" from the caller's side even though
        // rlcore's own log shows it did.
        ssize_t n = ::recvfrom(sock_, recv_buf_, sizeof(recv_buf_), 0,
                                reinterpret_cast<struct sockaddr*>(&src), &src_len);
        if (n <= 0) {
            return false; // timeout or error -- not fatal, loop again
        }

        DecodedFrame frame{};
        DecodeResult r = decode_frame(recv_buf_, static_cast<size_t>(n), &frame);
        if (r != DecodeResult::Ok) {
            // Not a data frame -- most likely a RegisterAck reply for a
            // register_with_rlcore_on_socket() call waiting on this same
            // socket. Hand it off instead of dropping so that call
            // doesn't lose the race for recvfrom() against this thread.
            {
                std::lock_guard<std::mutex> lock(register_reply_mutex_);
                if (register_reply_queue_.size() < kMaxQueuedRegisterReplies) {
                    register_reply_queue_.emplace(recv_buf_, recv_buf_ + n);
                }
            }
            register_reply_cv_.notify_one();
            return false; // drop silently, per spec
        }

        if (relay_dedup_enabled_ && is_recent_duplicate(frame.header.topic_id, frame.header.seq_num)) {
            return true; // exact duplicate (direct + relay both delivered it) -- drop
        }

        RawTopicCallback cb;
        {
            std::lock_guard<std::mutex> lock(handlers_mutex_);
            auto it = handlers_.find(frame.header.topic_id);
            if (it != handlers_.end()) cb = it->second;
        }
        if (cb) {
            cb(frame.payload, frame.payload_len); // inline on data thread, per spec
            return true;
        }
        return false; // no subscriber for this topic -- drop
    }

    int sock_ = -1;
    bool want_large_buffers_ = false;
    uint16_t local_port_ = 0;
    std::atomic<bool> running_{false};
    std::thread thread_;
    int pin_to_core_ = -1;
    bool use_realtime_ = false;
    int rt_priority_ = 10;
    std::atomic<uint16_t> seq_counter_{0};

    std::mutex handlers_mutex_;
    std::unordered_map<uint32_t, RawTopicCallback> handlers_;

    bool relay_dedup_enabled_ = false;

    // Remembers the last few delivered seq_nums per topic, not just the
    // most recent one: a relay copy can legitimately arrive AFTER
    // several newer direct messages already advanced past it (the relay
    // hop adds real latency), so "only equal to the single last one"
    // would miss it. A small fixed window (checked by linear scan --
    // cheap at this size) catches a duplicate arriving reasonably out
    // of order without unbounded memory growth.
    struct SeqWindow {
        static constexpr size_t kSize = 8;
        uint16_t seen[kSize] = {};
        bool valid[kSize] = {};
        size_t next = 0;
    };
    std::unordered_map<uint32_t, SeqWindow> recent_seqs_; // topic_id -> window, recv-thread-only

    bool is_recent_duplicate(uint32_t topic_id, uint16_t seq) {
        auto& w = recent_seqs_[topic_id];
        for (size_t i = 0; i < SeqWindow::kSize; ++i) {
            if (w.valid[i] && w.seen[i] == seq) return true;
        }
        w.seen[w.next] = seq;
        w.valid[w.next] = true;
        w.next = (w.next + 1) % SeqWindow::kSize;
        return false;
    }

    uint8_t send_buf_[kMaxFrameBytes];
    // 65507 (largest possible UDP/IPv4 datagram), not kMaxFrameBytes --
    // see the note in recv_and_dispatch() above.
    uint8_t recv_buf_[65507];

    // Handoff queue for non-frame packets (RegisterAck replies) received
    // by this thread but meant for a register_with_rlcore_on_socket()
    // call blocked in get_register_reply(). Bounded so a rlcore that's
    // gone haywire (or a misbehaving peer spamming garbage) can't grow
    // this unboundedly on a node that never calls get_register_reply().
    static constexpr size_t kMaxQueuedRegisterReplies = 16;
    std::mutex register_reply_mutex_;
    std::condition_variable register_reply_cv_;
    std::queue<std::vector<uint8_t>> register_reply_queue_;
};

// Helper: resolve a dotted-quad IPv4 string to host-order uint32_t.
inline uint32_t ipv4_to_host_order(const std::string& dotted) {
    struct in_addr addr{};
    if (::inet_pton(AF_INET, dotted.c_str(), &addr) != 1) {
        throw std::runtime_error("invalid IPv4 address: " + dotted);
    }
    return ntohl(addr.s_addr);
}

} // namespace relink

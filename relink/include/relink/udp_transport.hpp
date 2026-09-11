// Step 3 (part 2): raw UDP send/receive core, running on its own
// dedicated data thread, per relink-com-spec.md's "Threading and core
// allocation" section. This step builds only the data path
// (secure=false, checksum=false); discovery (com-core/multicast) is
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
#include <cstring>
#include <stdexcept>
#include <string>

#include <sys/socket.h>
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

        // A multi-chunk Image burst (advertise_image/publish_image) can
        // land hundreds of datagrams back-to-back faster than a single
        // recvfrom()+dispatch() cycle can drain them; Linux's default
        // SO_RCVBUF (often ~212KB) overflows well before a 640x480 raw
        // frame's ~664 chunks are drained, silently dropping the tail of
        // the burst (a dropped chunk drops the whole image, per Image's
        // no-retransmission design). Request a much larger buffer so a
        // full burst fits in the kernel queue; best-effort only -- if the
        // OS clamps it (e.g. net.core.rmem_max), that's fine, this is
        // strictly better than the default, never worse.
        int bufsize = 4 * 1024 * 1024;
        ::setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
        ::setsockopt(sock_, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));

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

    // Exposes the underlying socket fd, needed ONLY so callers (namely
    // RelinkNode's com-core registration step) can reuse this exact
    // socket for a pre-start() synchronous request/reply, keeping the
    // NAT-mapped source port com-core observes consistent with the port
    // this transport will actually receive data on. Safe to use before
    // start() launches the dedicated data thread; not meant for general
    // use once the thread is running (it owns recv from that point on).
    int native_handle() const { return sock_; }

    // Register the raw-bytes handler for a topic_id. Must be called
    // before start() for the topics this node will receive traffic on
    // (v1: no locking needed here since registration happens before the
    // data thread starts, consistent with the spec's "type fixed per
    // topic at registration" design).
    void set_topic_handler(uint16_t topic_id, RawTopicCallback cb) {
        std::lock_guard<std::mutex> lock(handlers_mutex_);
        handlers_[topic_id] = std::move(cb);
    }

    // Send one datagram to `peer`. Returns false (and does not send) if
    // payload_len exceeds the MTU budget -- reject loudly, never
    // silently truncate/fragment, per spec.
    bool publish_raw(uint16_t topic_id, const void* payload, size_t payload_len,
                      const PeerAddr& peer) {
        size_t frame_len = 0;
        uint16_t seq = seq_counter_.fetch_add(1, std::memory_order_relaxed);
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

private:
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
        ssize_t n = ::recvfrom(sock_, recv_buf_, sizeof(recv_buf_), 0,
                                reinterpret_cast<struct sockaddr*>(&src), &src_len);
        if (n <= 0) {
            return false; // timeout or error -- not fatal, loop again
        }

        DecodedFrame frame{};
        DecodeResult r = decode_frame(recv_buf_, static_cast<size_t>(n), &frame);
        if (r != DecodeResult::Ok) {
            return false; // drop silently, per spec
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
    uint16_t local_port_ = 0;
    std::atomic<bool> running_{false};
    std::thread thread_;
    int pin_to_core_ = -1;
    bool use_realtime_ = false;
    int rt_priority_ = 10;
    std::atomic<uint16_t> seq_counter_{0};

    std::mutex handlers_mutex_;
    std::unordered_map<uint16_t, RawTopicCallback> handlers_;

    uint8_t send_buf_[kMaxFrameBytes];
    uint8_t recv_buf_[kMaxFrameBytes];
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

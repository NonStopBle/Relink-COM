// Step 6: the public ReLink API -- RelinkNode, wiring together the ring
// buffer (step 2), UDP data thread (step 3), rlcore client (step 4),
// and multicast discovery (step 5) behind the templated
// advertise/subscribe/publish<T> surface shown in relink_example.cpp.

#pragma once

#include "relink/wire.hpp"
#include "relink/udp_transport.hpp"
#include "relink/rlcore_client.hpp"
#include "relink/multicast_discovery.hpp"
#include "relink/image.hpp"
#include "relink/topic_hash.hpp"
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <mutex>
#include <atomic>
#include <stdexcept>
#include <string>
#include <chrono>
#include <thread>
#include <memory>
#include <cstring>
#include <unistd.h>

namespace relink {

enum class DiscoveryMode { None, RlCore, Multicast };

class RelinkNode {
public:
    // --- mode A config sub-object, per spec: separate ip()/port()
    // setters, port has a default, mode selection happens at the ip()
    // call itself (the setter call, not deferred to spin()/start()). ---
    class RlCoreConfig {
    public:
        explicit RlCoreConfig(RelinkNode& owner) : owner_(owner) {}

        void ip(const std::string& addr) {
            owner_.select_mode(DiscoveryMode::RlCore);
            rlcore_ip_ = ipv4_to_host_order(addr);
            ip_set_ = true;
        }

        void port(uint16_t p = kRlCoreDefaultPort) {
            rlcore_port_ = p;
        }

        bool ip_is_set() const { return ip_set_; }
        uint32_t resolved_ip() const { return rlcore_ip_; }
        uint16_t resolved_port() const { return rlcore_port_; }

    private:
        RelinkNode& owner_;
        bool ip_set_ = false;
        uint32_t rlcore_ip_ = 0;
        uint16_t rlcore_port_ = kRlCoreDefaultPort;
    };

    RelinkNode() : set_rlcore(*this) {}

    RlCoreConfig set_rlcore;

    // --- mode B, mutually exclusive with mode A (per spec) ---
    void use_multicast_discovery() {
        select_mode(DiscoveryMode::Multicast);
    }

    // --- named topics: hash a human-readable topic name down to the
    // uint32_t that actually goes on the wire. One-way (FNV-1a) -- there
    // is no length limit on `name` since it is never itself transmitted,
    // but see topic_hash.hpp for why this can't be losslessly inverted.
    // "Decoding" an id back to a name only works locally, via the
    // registry this call populates (topic_name_for()), and only for
    // names this process itself has advertised/subscribed/published.
    //
    // Throws on a genuine hash collision (two different names landing on
    // the same 32-bit id) or if the name hashes to the reserved NAT-punch
    // id -- both are astronomically unlikely for realistic topic counts,
    // but must never be silently ignored per ReLink's "never
    // misinterpret bytes" rule.
    uint32_t topic_id_for(const std::string& name) {
        uint32_t id = fnv1a32(name);
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (id == kNatPunchTopicId) {
            throw std::runtime_error("ReLink: topic name \"" + name +
                "\" hashes to the reserved NAT-punch topic id -- pick a different name");
        }
        auto it = topic_names_.find(id);
        if (it != topic_names_.end()) {
            if (it->second != name) {
                throw std::runtime_error("ReLink: topic hash collision between \"" +
                    it->second + "\" and \"" + name + "\" (both hash to " + std::to_string(id) + ")");
            }
        } else {
            topic_names_.emplace(id, name);
        }
        return id;
    }

    // Reverse lookup into this process's own registry (built only from
    // names it has itself resolved via topic_id_for/the string
    // overloads below) -- empty string if this id was never named here.
    std::string topic_name_for(uint32_t topic_id) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = topic_names_.find(topic_id);
        return it != topic_names_.end() ? it->second : std::string();
    }

    // One entry describing a topic this node has declared (via
    // advertise/subscribe, including the *_image variants), for
    // rltopic_list() below.
    struct RlTopicInfo {
        uint32_t topic_id;
        std::string name; // empty if this topic was declared by numeric
                            // id directly (topic_id_for() was never
                            // called for it, so no name is known locally)
    };

    // Lists every topic this node has advertised or subscribed to so
    // far, with its human name when known (from the string-based
    // advertise/subscribe/publish overloads) -- a debugging/introspection
    // aid, not part of the wire protocol. Does NOT list topics only
    // known as a remote peer's publish target (see peers_for_topic()
    // for that); this is specifically "what has THIS node declared."
    std::vector<RlTopicInfo> rltopic_list() {
        std::lock_guard<std::mutex> lock(state_mutex_);
        std::vector<RlTopicInfo> out;
        out.reserve(declared_topics_.size());
        for (uint32_t id : declared_topics_) {
            auto it = topic_names_.find(id);
            out.push_back(RlTopicInfo{id, it != topic_names_.end() ? it->second : std::string()});
        }
        return out;
    }

    // --- advertise: publisher-side topic declaration ---
    template <typename T>
    void advertise(const std::string& name, bool secure = false, bool checksum = false) {
        advertise<T>(topic_id_for(name), secure, checksum);
    }

    template <typename T>
    void advertise(uint32_t topic_id, bool /*secure*/ = false, bool /*checksum*/ = false) {
        static_assert(std::is_trivially_copyable<T>::value,
                      "advertise<T>: T must be trivially copyable");
        if constexpr (std::is_same<T, ImageChunk>::value) {
            transport_.enable_large_buffers();
        }
        std::lock_guard<std::mutex> lock(state_mutex_);
        declared_topics_.insert(topic_id);
    }

    // --- subscribe: receiver-side topic declaration + typed callback,
    // invoked inline on the data thread per spec's v1 threading design ---
    template <typename T, typename Callback>
    void subscribe(const std::string& name, Callback callback, bool secure = false) {
        subscribe<T>(topic_id_for(name), std::move(callback), secure);
    }

    template <typename T, typename Callback>
    void subscribe(uint32_t topic_id, Callback callback, bool /*secure*/ = false) {
        static_assert(std::is_trivially_copyable<T>::value,
                      "subscribe<T>: T must be trivially copyable");
        if constexpr (std::is_same<T, ImageChunk>::value) {
            transport_.enable_large_buffers();
        }
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            declared_topics_.insert(topic_id);
        }
        transport_.set_topic_handler(topic_id, [callback](const uint8_t* payload, size_t len) {
            if (len != sizeof(T)) return; // type/size mismatch: drop, never misinterpret bytes
            T value{};
            std::memcpy(&value, payload, sizeof(T));
            callback(value);
        });
    }

    // --- publish: sends to every currently-known peer for this topic ---
    template <typename T>
    bool publish(const std::string& name, const T& value) {
        return publish<T>(topic_id_for(name), value);
    }

    template <typename T>
    bool publish(uint32_t topic_id, const T& value) {
        static_assert(std::is_trivially_copyable<T>::value,
                      "publish<T>: T must be trivially copyable");
        ensure_started();

        std::vector<PeerAddr> peers;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            auto it = peers_.find(topic_id);
            if (it != peers_.end()) peers = it->second;
        }
        bool all_ok = true;
        for (const auto& peer : peers) {
            all_ok = transport_.publish_raw(topic_id, &value, sizeof(T), peer) && all_ok;
        }
        return all_ok;
    }

    // --- Image: a library-provided large-blob type, automatically
    // chunked to the MTU maximum on send and reassembled on receive.
    // See image.hpp -- built entirely on the same publish<T>/subscribe<T>
    // machinery above, one ordinary ImageChunk message per datagram, not
    // a new wire mechanism. Not JPEG/PNG-specific: carries whatever
    // bytes you give it. ---

    // advertise_image is just advertise<ImageChunk> under a clearer name
    // for this use case -- both are equivalent to call.
    void advertise_image(const std::string& name) { advertise_image(topic_id_for(name)); }
    void advertise_image(uint32_t topic_id) { advertise<ImageChunk>(topic_id); }

    // Splits data/len into MTU-maximized chunks and publishes each one
    // in order. frame_id lets the receiver match chunks belonging to the
    // same image and is auto-incremented per topic if not supplied.
    // Returns false if `len` exceeds kMaxImageBytes (chunk_count would
    // overflow uint16_t) -- rejected loudly, per ReLink's "never
    // silently truncate" rule, same as publish<T> rejecting an oversized
    // fixed message.
    // Zero-copy send path: unlike encode_image_chunks() (still used by
    // the pure encode/reassemble unit tests and available for anyone
    // building their own transport), this never copies the caller's
    // image bytes into an intermediate ImageChunk struct -- each
    // chunk's small header and the caller's own data slice are handed
    // straight to the kernel via UdpTransport::publish_scattered()'s
    // sendmsg() scatter-gather, so a several-hundred-chunk burst costs
    // one userspace copy fewer per chunk than going through publish<T>.
    bool publish_image(const std::string& name, const uint8_t* data, size_t len,
                        uint32_t frame_id = kAutoFrameId) {
        return publish_image(topic_id_for(name), data, len, frame_id);
    }

    bool publish_image(uint32_t topic_id, const uint8_t* data, size_t len,
                        uint32_t frame_id = kAutoFrameId) {
        if (len > kMaxImageBytes) return false;
        if (frame_id == kAutoFrameId) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            frame_id = next_image_frame_id_[topic_id]++;
        }
        ensure_started();

        std::vector<PeerAddr> peers;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            auto it = peers_.find(topic_id);
            if (it != peers_.end()) peers = it->second;
        }

        uint16_t chunk_count = static_cast<uint16_t>(
            (len + kImageChunkDataBytes - 1) / kImageChunkDataBytes);
        if (chunk_count == 0) chunk_count = 1; // empty image is still one chunk

        bool all_ok = true;
        for (uint16_t i = 0; i < chunk_count; ++i) {
#pragma pack(push, 1)
            struct ChunkHeader {
                uint32_t frame_id;
                uint16_t chunk_index;
                uint16_t chunk_count;
                uint16_t chunk_bytes;
            };
#pragma pack(pop)
            static_assert(sizeof(ChunkHeader) == kImageChunkHeaderBytes,
                          "ChunkHeader must exactly match the wire chunk header layout");
            ChunkHeader chdr{frame_id, i, chunk_count, 0};
            size_t offset = size_t(i) * kImageChunkDataBytes;
            size_t n = std::min(kImageChunkDataBytes, len - offset);
            chdr.chunk_bytes = static_cast<uint16_t>(n);

            for (const auto& peer : peers) {
                all_ok = transport_.publish_scattered(topic_id, &chdr, sizeof(chdr),
                                                        data + offset, n, peer) && all_ok;
            }
        }
        return all_ok;
    }

    // Subscribes to a topic of Image chunks; `callback` fires once per
    // COMPLETE image (not once per chunk) with the reassembled bytes. An
    // image whose chunks arrive incompletely before the next one starts
    // is silently dropped -- no retransmission, matching ReLink's UDP
    // design throughout (see image.hpp's ImageReassembler and
    // examples/cpp/camera_stream.cpp's measured reliability numbers).
    //
    // IMPORTANT: like every subscribe<T>() callback, this runs inline on
    // ReLink's one data thread -- a slow callback (JPEG decode, disk
    // I/O, ML inference) blocks recv() from draining the socket at all.
    // The socket's larger receive buffer (see udp_transport.hpp) buys
    // some slack for a short burst, but it is NOT a substitute for a
    // fast callback: measured with a 50ms/frame callback against a
    // faster publisher, per-frame latency grew linearly and delivery
    // eventually collapsed once the backlog outran the buffer. If your
    // work is slow, hand it off to your own worker thread/queue instead
    // of doing it here.
    template <typename Callback>
    void subscribe_image(const std::string& name, Callback callback) {
        subscribe_image(topic_id_for(name), std::move(callback));
    }

    template <typename Callback>
    void subscribe_image(uint32_t topic_id, Callback callback) {
        transport_.enable_large_buffers();
        auto reassembler = std::make_shared<ImageReassembler>(
            [callback](uint32_t frame_id, const std::vector<uint8_t>& image) {
                callback(frame_id, image);
            });
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            image_reassemblers_.push_back(reassembler); // keep alive for node lifetime
            declared_topics_.insert(topic_id);
        }
        // Zero-copy receive path: publish_image() sends the compact
        // wire format (10-byte header + only the valid data bytes, not
        // padded to kImageChunkDataBytes), so this parses that directly
        // out of the raw recv buffer instead of going through
        // subscribe<ImageChunk>() (which requires payload_len ==
        // sizeof(ImageChunk) exactly and would reject every partial
        // last chunk).
        transport_.set_topic_handler(topic_id, [reassembler](const uint8_t* payload, size_t len) {
            if (len < kImageChunkHeaderBytes) return;
            uint32_t frame_id; uint16_t chunk_index, chunk_count, chunk_bytes;
            std::memcpy(&frame_id, payload, sizeof(frame_id));
            std::memcpy(&chunk_index, payload + 4, sizeof(chunk_index));
            std::memcpy(&chunk_count, payload + 6, sizeof(chunk_count));
            std::memcpy(&chunk_bytes, payload + 8, sizeof(chunk_bytes));
            const uint8_t* data = payload + kImageChunkHeaderBytes;
            size_t data_len = len - kImageChunkHeaderBytes;
            if (chunk_bytes != data_len) return; // mismatch: drop, never misinterpret bytes
            reassembler->on_chunk_raw(frame_id, chunk_index, chunk_count, data, chunk_bytes);
        });
    }

    // Runs discovery/data setup if not already done, then returns
    // immediately -- v1 keeps recv/dispatch on UdpTransport's own
    // dedicated data thread (started here), so spin_once() has no
    // per-call work of its own beyond the one-time setup.
    void spin_once() {
        ensure_started();
    }

    // Blocks (servicing nothing extra itself, since the data thread
    // already runs independently) until request_stop() is called.
    void spin() {
        ensure_started();
        while (!stop_requested_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    void request_stop() { stop_requested_ = true; }

    // Override the data thread's CPU pin (see Lean optimization section
    // of the spec). Pass -1 to disable pinning entirely; must be called
    // before the first spin()/spin_once()/publish() call (i.e. before
    // ensure_started() runs) to take effect.
    static constexpr int kAutoPinCore = -2;
    void set_data_thread_core(int core) { data_thread_core_override_ = core; }

    // Opt-in SCHED_FIFO for the data thread (spec: per-thread only, apply
    // "with care"). Degrades gracefully (logs, keeps normal scheduling)
    // if the process lacks the privilege to set it -- never fatal.
    void set_data_thread_realtime(bool enabled, int priority = 10) {
        use_realtime_ = enabled;
        rt_priority_ = priority;
    }

    uint16_t local_data_port() {
        ensure_started();
        return transport_.local_port();
    }

    // Snapshot of currently-known peers for a topic (test/debug use).
    std::vector<PeerAddr> peers_for_topic(uint32_t topic_id) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = peers_.find(topic_id);
        return it != peers_.end() ? it->second : std::vector<PeerAddr>{};
    }

private:
    friend class RlCoreConfig;

    void select_mode(DiscoveryMode requested) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (mode_ != DiscoveryMode::None && mode_ != requested) {
            throw std::runtime_error(
                "discovery mode already set; cannot enable a second, mutually "
                "exclusive discovery mode on the same node");
        }
        mode_ = requested;
    }

    void ensure_started() {
        std::lock_guard<std::mutex> lock(start_mutex_);
        if (started_) return;

        DiscoveryMode mode;
        {
            std::lock_guard<std::mutex> lock2(state_mutex_);
            mode = mode_;
        }
        if (mode == DiscoveryMode::None) {
            throw std::runtime_error(
                "no discovery method configured -- call set_rlcore.ip(...) or "
                "use_multicast_discovery() before spin()/publish()/subscribe traffic");
        }
        if (mode == DiscoveryMode::RlCore && !set_rlcore.ip_is_set()) {
            throw std::runtime_error("rlcore IP not set -- call set_rlcore.ip(...)");
        }

        transport_.bind(0);

        std::vector<uint32_t> topics;
        {
            std::lock_guard<std::mutex> lock2(state_mutex_);
            topics.assign(declared_topics_.begin(), declared_topics_.end());
        }

        std::vector<PeerAddr> newly_learned_peers; // for the NAT punch burst below

        if (mode == DiscoveryMode::RlCore) {
            // Registration MUST happen on transport_'s own socket, before
            // transport_.start() hands that socket's recv loop to the
            // dedicated data thread (two threads calling recvfrom() on
            // the same fd concurrently would race the ack reply against
            // the data thread's recv_and_dispatch). This also has a
            // second purpose beyond avoiding that race: when rlcore is
            // run with --nat, it learns each node's real (NAT-mapped)
            // public endpoint from the register request's UDP source
            // port -- that's only useful/correct if it's the SAME port
            // the node's data traffic actually arrives on, i.e. this
            // socket, not a throwaway one.
            uint32_t self_ip = detect_local_ip_for_peer(set_rlcore.resolved_ip(),
                                                         set_rlcore.resolved_port());
            auto outcome = register_with_rlcore_on_socket(
                transport_.native_handle(),
                set_rlcore.resolved_ip(), set_rlcore.resolved_port(),
                self_ip, transport_.local_port(),
                topics.data(), static_cast<uint16_t>(topics.size()));
            if (outcome.ok) {
                std::lock_guard<std::mutex> lock2(state_mutex_);
                for (const auto& p : outcome.peers) {
                    PeerAddr addr{p.ip, p.port};
                    peers_[p.topic_id].push_back(addr);
                    newly_learned_peers.push_back(addr);
                }
            }
            // If registration failed after retries, register_with_rlcore
            // already logged an error; proceed with an empty peer table
            // rather than crashing the node (spec: never hang forever).
        } else {
            // Multicast: self_ip auto-detected via the multicast group
            // address as the "peer" for route selection.
            MulticastDiscoveryConfig cfg;
            cfg.self_ip = detect_local_ip_for_peer(
                ipv4_to_host_order(kDefaultMulticastGroup), kDefaultMulticastPort);
            cfg.self_data_port = transport_.local_port();
            cfg.local_topics = topics;
            mcast_ = std::make_unique<MulticastDiscovery>(cfg);
            mcast_->set_peer_discovered_callback([this](uint32_t topic, const PeerInfo& p) {
                std::lock_guard<std::mutex> lock2(state_mutex_);
                peers_[topic].push_back(PeerAddr{p.ip, p.port});
            });
            mcast_->start();
        }

        // Pin the data thread to a dedicated core, per spec's Threading/
        // Lean-optimization sections ("the single highest-leverage fix
        // for tail-latency spikes"). NOT auto-pinned by default: per
        // spec, "if a robot runs multiple ReLink nodes on the same
        // board... be deliberate about which core each node's data
        // thread pins to -- two data threads fighting for the same
        // pinned core reintroduces the exact scheduling jitter pinning
        // was meant to eliminate." An auto-picked default (e.g. "last
        // core") cannot safely avoid that collision across independent
        // processes with no shared coordination, so pinning here is
        // opt-in only via set_data_thread_core(core).
        int pin_core = (data_thread_core_override_ == kAutoPinCore) ? -1 : data_thread_core_override_;
        transport_.start(pin_core, use_realtime_, rt_priority_);

        // NAT hole punching: fire a small burst of empty datagrams at
        // every peer learned from this registration. This only matters
        // (and is only correct) when rlcore is run with --nat, which
        // makes it hand out each peer's real internet-facing endpoint
        // instead of their self-reported LAN address -- sending a
        // datagram FROM this node TO that endpoint opens this node's own
        // NAT's outbound mapping, so the peer's (simultaneous) punch
        // datagram back can get through. Harmless no-op cost on a plain
        // LAN (a handful of tiny UDP packets to addresses already
        // reachable directly). Uses the reserved kNatPunchTopicId, which
        // every node silently drops on receive since nothing ever
        // subscribes to it.
        for (const auto& peer : newly_learned_peers) {
            for (int i = 0; i < 3; ++i) {
                transport_.publish_raw(kNatPunchTopicId, nullptr, 0, peer);
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
            }
        }

        started_ = true;
    }

    static uint32_t detect_local_ip_for_peer(uint32_t peer_ip_host, uint16_t peer_port) {
        int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) throw std::runtime_error("detect_local_ip_for_peer: socket() failed");

        struct sockaddr_in peer{};
        peer.sin_family = AF_INET;
        peer.sin_addr.s_addr = htonl(peer_ip_host);
        peer.sin_port = htons(peer_port);

        uint32_t local_ip = 0;
        if (::connect(sock, reinterpret_cast<struct sockaddr*>(&peer), sizeof(peer)) == 0) {
            struct sockaddr_in local{};
            socklen_t len = sizeof(local);
            if (::getsockname(sock, reinterpret_cast<struct sockaddr*>(&local), &len) == 0) {
                local_ip = ntohl(local.sin_addr.s_addr);
            }
        }
        ::close(sock);
        return local_ip;
    }

    static constexpr uint32_t kAutoFrameId = 0xFFFFFFFF;

    std::mutex state_mutex_;
    DiscoveryMode mode_ = DiscoveryMode::None;
    std::unordered_set<uint32_t> declared_topics_;
    std::unordered_map<uint32_t, std::vector<PeerAddr>> peers_;
    std::unordered_map<uint32_t, uint32_t> next_image_frame_id_;
    std::unordered_map<uint32_t, std::string> topic_names_;
    std::vector<std::shared_ptr<ImageReassembler>> image_reassemblers_;

    std::mutex start_mutex_;
    bool started_ = false;
    int data_thread_core_override_ = kAutoPinCore;
    bool use_realtime_ = false;
    int rt_priority_ = 10;
    std::atomic<bool> stop_requested_{false};

    UdpTransport transport_;
    std::unique_ptr<MulticastDiscovery> mcast_;
};

} // namespace relink

// Bring default types and RelinkNode into the global namespace to match
// relink_example.cpp / relink_benchmark.cpp's unqualified usage
// (`RelinkNode`, `Float32`, etc.) -- those files #include "relink/relink.hpp"
// and use the types without a `relink::` prefix.
using relink::RelinkNode;
using relink::Bool;
using relink::Byte;
using relink::Char;
using relink::Int8;
using relink::Int16;
using relink::Int32;
using relink::Int64;
using relink::UInt8;
using relink::UInt16;
using relink::UInt32;
using relink::UInt64;
using relink::Float32;
using relink::Float64;

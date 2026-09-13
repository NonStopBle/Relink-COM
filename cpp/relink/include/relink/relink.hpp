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
#include "relink/topic_directory.hpp"
#include "relink/standard_msgs.hpp"
#include "relink/relay_wire.hpp"
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
#include <functional>

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

    // Multiplex (default, true): every topic shares this node's one UDP
    // socket/port, demultiplexed by topic_id in UdpTransport's handler
    // map -- the design this library is built around (one socket per
    // NODE, not per topic; see udp_transport.hpp's header comment). The
    // ~26ns handler-map lookup this costs per received frame is dwarfed
    // by the ~1-5us cost of a syscall, so per-topic sockets would not
    // make a single node faster -- but they DO match ROS's familiar
    // one-port-per-topic model, which some deployments want for its own
    // sake (e.g. per-topic firewall rules, or tooling that expects to
    // find a topic on its own port). set_multiplex(false) opts a node
    // into that model: each advertise/subscribe/advertise_raw/
    // subscribe_raw call after this is set gets its own dedicated
    // UdpTransport bound to its own ephemeral port. Discovery (rlcore or
    // multicast) automatically announces each topic's real port instead
    // of one shared port -- no separate opt-in needed on rlcore's side,
    // since a registration/beacon already carries an explicit port
    // alongside whichever topics it lists (see MulticastDiscoveryConfig::
    // PortGroup and ensure_started() below). Must be called before the
    // first advertise/subscribe/publish call it should affect. Image
    // (advertise_image/publish_image/subscribe_image) always stays on
    // the shared transport regardless of this setting.
    void set_multiplex(bool enabled) { multiplex_ = enabled; }

    ~RelinkNode() {
        repunch_stop_ = true;
        if (repunch_thread_.joinable()) repunch_thread_.join();
        relay_stop_ = true;
        if (relay_keepalive_thread_.joinable()) relay_keepalive_thread_.join();
    }

    // Opt-in background re-punch: instead of firing the NAT hole-punch
    // burst only once, when a peer is first learned, keep re-punching
    // every known peer on a timer for the node's whole lifetime. Fixes
    // a real but narrow class of failure -- a marginal NAT whose mapping
    // expires faster than expected, or a peer discovered on one side
    // just before the other side's mapping timed out -- by refreshing
    // every mapping before it can expire. It does NOT fix a NAT/firewall
    // that structurally drops all unsolicited inbound UDP regardless of
    // timing (verified against a real mobile-carrier NAT: 85 retries
    // over 22 seconds still delivered zero packets) -- no amount of
    // retrying opens a path that was never open. Call before spin()/
    // publish() traffic; harmless no-op cost on a plain LAN, same as the
    // one-shot burst it supplements.
    void enable_nat_repunch(double interval_seconds = 5.0) {
        repunch_enabled_ = true;
        repunch_interval_ = std::chrono::duration<double>(interval_seconds);
    }

    // Relay fallback for when direct peer-to-peer hole punching cannot
    // cross a NAT/firewall at all -- a real, verified failure mode (see
    // README Step 13): some NATs (mobile carriers especially) drop
    // unsolicited inbound UDP from a third party regardless of punch
    // timing. A relay works there because both clients only ever open a
    // NAT mapping toward the relay's one fixed (ip, port), never toward
    // each other -- the relay's replies always come from that exact
    // remote endpoint, which every stateful NAT/firewall allows back in,
    // by definition.
    //
    // This is a redundant SECOND path, not a detect-failure-then-switch
    // one: once enabled, every publish also goes to the relay, and
    // every topic's socket also registers with (and is kept alive at)
    // the relay, all the time -- direct punching still runs exactly as
    // before. This trades a bit of extra relay bandwidth for not having
    // to reliably detect "did direct delivery actually work", which
    // would need an ack protocol; a subscriber that gets the same
    // message from both paths silently drops the second copy (matched
    // by seq_num, see UdpTransport::enable_relay_dedup()), so enabling
    // this is safe to leave on rather than something to toggle per
    // failure. Call before spin()/publish() traffic; must be paired
    // with a relink-relay daemon running at `ip:port`.
    void set_relay(const std::string& ip, uint16_t port = kRelayDefaultPort) {
        relay_enabled_ = true;
        relay_ip_ = ipv4_to_host_order(ip);
        relay_port_ = port;
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

    // Lists every topic id THIS node has itself declared (advertised or
    // subscribed to), PLUS -- when using multicast discovery (mode B) --
    // every topic id any other node's beacon has announced, network-wide
    // (the rostopic-list-style view). Beacons only ever carry the 4-byte
    // numeric id, never a name (see multicast_discovery.hpp), so a topic
    // learned purely from the network has an empty name here; it only
    // gets a name if THIS process separately resolved that same id via
    // topic_id_for() (i.e. it also advertised/subscribed that name
    // itself) -- "decoding" a name is always local, never transmitted.
    // Mode A (rlcore) does not currently feed this list beyond what this
    // node declared -- the daemon doesn't broadcast a topic roster back.
    std::vector<RlTopicInfo> rltopic_list() {
        std::unordered_set<uint32_t> ids;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            ids.insert(declared_topics_.begin(), declared_topics_.end());
        }
        if (mcast_) {
            for (uint32_t id : mcast_->all_known_topic_ids()) ids.insert(id);
        }

        std::lock_guard<std::mutex> lock(state_mutex_);
        std::vector<RlTopicInfo> out;
        out.reserve(ids.size());
        for (uint32_t id : ids) {
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
            transport_.enable_large_buffers(); // Image always stays on the shared transport
        } else {
            transport_for(topic_id);
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
        UdpTransport& t = std::is_same<T, ImageChunk>::value ? transport_ : transport_for(topic_id);
        if constexpr (std::is_same<T, ImageChunk>::value) {
            transport_.enable_large_buffers();
        }
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            declared_topics_.insert(topic_id);
        }
        t.set_topic_handler(topic_id, [callback](const uint8_t* payload, size_t len) {
            if (len != sizeof(T)) return; // type/size mismatch: drop, never misinterpret bytes
            T value{};
            std::memcpy(&value, payload, sizeof(T));
            callback(value);
        });
    }

    // --- subscribe_raw/publish_raw: type-agnostic escape hatch, for
    // tooling that inspects a topic without knowing its message type
    // (rl_topic's echo/hz/bw subcommands -- see rl_topic.cpp/.py) --
    // NOT for application code, which should always use the typed
    // advertise/subscribe/publish<T> above so a size mismatch is caught
    // per ReLink's "never misinterpret bytes" rule instead of being
    // handed to you as unstructured bytes.
    using RawCallback = std::function<void(const uint8_t* payload, size_t len)>;

    void advertise_raw(const std::string& name) { advertise_raw(topic_id_for(name)); }
    void advertise_raw(uint32_t topic_id) {
        transport_for(topic_id);
        std::lock_guard<std::mutex> lock(state_mutex_);
        declared_topics_.insert(topic_id);
    }

    void subscribe_raw(const std::string& name, RawCallback callback) {
        subscribe_raw(topic_id_for(name), std::move(callback));
    }
    void subscribe_raw(uint32_t topic_id, RawCallback callback) {
        UdpTransport& t = transport_for(topic_id);
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            declared_topics_.insert(topic_id);
        }
        t.set_topic_handler(topic_id, std::move(callback));
    }

    bool publish_raw(const std::string& name, const void* payload, size_t payload_len) {
        return publish_raw(topic_id_for(name), payload, payload_len);
    }
    bool publish_raw(uint32_t topic_id, const void* payload, size_t payload_len) {
        ensure_started();
        std::vector<PeerAddr> peers;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            declared_topics_.insert(topic_id);
            auto it = peers_.find(topic_id);
            if (it != peers_.end()) peers = it->second;
        }
        UdpTransport& t = transport_for(topic_id);
        uint16_t seq = t.next_seq(); // shared across every direct peer AND the relay copy
        bool all_ok = true;
        for (const auto& peer : peers) {
            all_ok = t.publish_raw(topic_id, payload, payload_len, peer, seq) && all_ok;
        }
        if (relay_enabled_) t.publish_raw(topic_id, payload, payload_len, relay_peer(), seq);
        return all_ok && !peers.empty();
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
        UdpTransport& t = std::is_same<T, ImageChunk>::value ? transport_ : transport_for(topic_id);
        uint16_t seq = t.next_seq(); // shared across every direct peer AND the relay copy
        bool all_ok = true;
        for (const auto& peer : peers) {
            all_ok = t.publish_raw(topic_id, &value, sizeof(T), peer, seq) && all_ok;
        }
        if (relay_enabled_ && !std::is_same<T, ImageChunk>::value) {
            t.publish_raw(topic_id, &value, sizeof(T), relay_peer(), seq);
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

    // Returns the transport that topic_id should send/receive on: the
    // one shared transport_ in multiplexed mode (the default), or a
    // dedicated, lazily-created per-topic transport when
    // set_multiplex(false) is in effect. Must be called (directly or via
    // advertise/subscribe/advertise_raw/subscribe_raw) BEFORE
    // ensure_started() for the topic's dedicated port to be known in
    // time to register/beacon it -- same ordering requirement
    // declared_topics_ already has.
    UdpTransport& transport_for(uint32_t topic_id) {
        if (multiplex_) return transport_;
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = topic_transports_.find(topic_id);
        if (it != topic_transports_.end()) return *it->second;
        auto t = std::make_unique<UdpTransport>();
        t->bind(0);
        UdpTransport* raw = t.get();
        topic_transports_.emplace(topic_id, std::move(t));
        return *raw;
    }

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
        // One group per distinct transport this node's declared topics
        // ended up on: exactly one (the shared transport_) in the
        // default multiplexed mode; one per topic (each its own
        // dedicated transport, plus a leftover group for any topic that
        // for whatever reason never got one -- e.g. Image, which always
        // stays on transport_) when set_multiplex(false) is in effect.
        // This is the ONLY place multiplex vs. demultiplex changes
        // discovery behavior -- rlcore/multicast themselves don't need
        // to know which mode a node is in, since each registration/
        // beacon already carries its own explicit port alongside
        // whichever topics it lists.
        struct TopicGroup { UdpTransport* transport; std::vector<uint32_t> topics; };
        std::vector<TopicGroup> groups;
        {
            std::lock_guard<std::mutex> lock2(state_mutex_);
            topics.assign(declared_topics_.begin(), declared_topics_.end());
            if (!multiplex_ && !topic_transports_.empty()) {
                std::unordered_set<uint32_t> demuxed;
                for (auto& kv : topic_transports_) {
                    groups.push_back(TopicGroup{kv.second.get(), std::vector<uint32_t>{kv.first}});
                    demuxed.insert(kv.first);
                }
                std::vector<uint32_t> leftover;
                for (uint32_t id : declared_topics_) if (!demuxed.count(id)) leftover.push_back(id);
                if (!leftover.empty()) groups.push_back(TopicGroup{&transport_, leftover});
            } else if (!topics.empty()) {
                groups.push_back(TopicGroup{&transport_, topics});
            }
            for (const auto& g : groups)
                for (uint32_t t : g.topics) topic_route_[t] = g.transport;
            // Dedup MUST be armed before transport_.start() launches the
            // recv thread below, not after -- enabling it post-start
            // leaves a race window where an early direct+relay duplicate
            // pair can both slip through before the flag takes effect.
            if (relay_enabled_) {
                for (const auto& g : groups) g.transport->enable_relay_dedup();
            }
        }

        std::vector<std::pair<UdpTransport*, PeerAddr>> newly_learned_peers; // for the NAT punch burst below

        if (mode == DiscoveryMode::RlCore) {
            // Registration MUST happen on each group's own socket, before
            // that transport's start() hands its socket's recv loop to
            // its dedicated data thread (two threads calling recvfrom()
            // on the same fd concurrently would race the ack reply
            // against the data thread's recv_and_dispatch). This also has
            // a second purpose beyond avoiding that race: when rlcore is
            // run with --nat, it learns each node's real (NAT-mapped)
            // public endpoint from the register request's UDP source
            // port -- that's only useful/correct if it's the SAME port
            // that transport's data traffic actually arrives on, i.e.
            // this socket, not a throwaway one. One RegisterRequest is
            // sent per group, each with that group's own port -- rlcore
            // needs no code change to hand back the right per-topic port,
            // since it already stores whatever (ip, port, topic) triple
            // each request declares.
            uint32_t self_ip = detect_local_ip_for_peer(set_rlcore.resolved_ip(),
                                                         set_rlcore.resolved_port());
            for (const auto& g : groups) {
                auto outcome = register_with_rlcore_on_socket(
                    g.transport->native_handle(),
                    set_rlcore.resolved_ip(), set_rlcore.resolved_port(),
                    self_ip, g.transport->local_port(),
                    g.topics.data(), static_cast<uint16_t>(g.topics.size()));
                if (outcome.ok) {
                    std::lock_guard<std::mutex> lock2(state_mutex_);
                    for (const auto& p : outcome.peers) {
                        PeerAddr addr{p.ip, p.port};
                        peers_[p.topic_id].push_back(addr);
                        newly_learned_peers.push_back({g.transport, addr});
                    }
                }
            }
            // Tell rlcore about any names we resolved for these topics
            // (best-effort, fire-and-forget -- rl_topic.py's RLNQ query
            // to rlcore is what actually depends on this, not any
            // data-path behavior, so a dropped/lost announce here is
            // harmless: worst case rl_topic.py's dictionary is missing
            // one name until the next process that knows it announces).
            {
                std::vector<TopicDirEntry> entries;
                {
                    std::lock_guard<std::mutex> lock2(state_mutex_);
                    entries.reserve(topic_names_.size());
                    for (const auto& kv : topic_names_) entries.push_back(TopicDirEntry{kv.first, kv.second});
                }
                if (!entries.empty()) {
                    uint8_t announce_buf[kTopicDirMaxPacket];
                    size_t announce_len = 0;
                    if (encode_topic_dir_announce(entries, announce_buf, sizeof(announce_buf), &announce_len)) {
                        struct sockaddr_in dest{};
                        dest.sin_family = AF_INET;
                        dest.sin_addr.s_addr = htonl(set_rlcore.resolved_ip());
                        dest.sin_port = htons(set_rlcore.resolved_port());
                        ::sendto(transport_.native_handle(), announce_buf, announce_len, 0,
                                 reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest));
                    }
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
            for (const auto& g : groups) {
                cfg.port_groups.push_back(
                    MulticastDiscoveryConfig::PortGroup{g.transport->local_port(), g.topics});
            }
            mcast_ = std::make_unique<MulticastDiscovery>(cfg);
            mcast_->set_peer_discovered_callback([this](uint32_t topic, const PeerInfo& p) {
                PeerAddr addr{p.ip, p.port};
                UdpTransport* via;
                {
                    std::lock_guard<std::mutex> lock2(state_mutex_);
                    peers_[topic].push_back(addr);
                    auto it = topic_route_.find(topic);
                    via = (it != topic_route_.end()) ? it->second : &transport_;
                }
                // Multicast discovery keeps running for the node's whole
                // lifetime (unlike rlcore's one-shot registration burst
                // below), so a peer for a set_multiplex(false) topic can
                // show up long after start() -- punch from that topic's
                // OWN socket every time, not just at startup, or its NAT
                // mapping never opens and the peer's punch back is dropped.
                nat_punch(addr, *via);
            });
            // Answers rl_topic's name-directory queries (see
            // topic_directory.hpp) -- called from MulticastDiscovery's
            // own listener thread, so lock state_mutex_ ourselves rather
            // than relying on a caller that already holds it.
            mcast_->set_topic_name_provider([this]() {
                std::lock_guard<std::mutex> lock2(state_mutex_);
                std::vector<TopicDirEntry> out;
                out.reserve(topic_names_.size());
                for (const auto& kv : topic_names_) out.push_back(TopicDirEntry{kv.first, kv.second});
                return out;
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
        {
            // Demultiplexed topics each need their own data thread too --
            // set_topic_handler() was already called on these at
            // advertise/subscribe time, before this point.
            std::lock_guard<std::mutex> lock2(state_mutex_);
            for (auto& kv : topic_transports_) kv.second->start(pin_core, use_realtime_, rt_priority_);
        }

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
        for (const auto& kv : newly_learned_peers) {
            nat_punch(kv.second, *kv.first);
        }

        // NAT mode (rlcore, the only mode --nat applies to) gets a 1s
        // re-punch thread by default -- this is the mode where a peer's
        // real endpoint was learned from a NAT-mapped source port that
        // can drift/expire, so continuously rechecking readiness matters
        // enough to not require an opt-in call. A user who already
        // called enable_nat_repunch() themselves (any mode, any
        // interval) keeps their own setting -- this only fills in the
        // default when nothing was explicitly requested.
        if (mode == DiscoveryMode::RlCore && !repunch_enabled_) {
            repunch_enabled_ = true;
            repunch_interval_ = std::chrono::duration<double>(1.0);
        }
        if (repunch_enabled_) start_repunch_thread();

        if (relay_enabled_) {
            std::vector<std::pair<UdpTransport*, std::vector<uint32_t>>> relay_groups;
            relay_groups.reserve(groups.size());
            for (const auto& g : groups) relay_groups.emplace_back(g.transport, g.topics);
            register_all_topics_with_relay(relay_groups); // immediate, don't wait for the first keepalive tick
            start_relay_keepalive_thread(std::move(relay_groups));
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

    bool multiplex_ = true;
    std::unordered_map<uint32_t, std::unique_ptr<UdpTransport>> topic_transports_;
    // Which transport owns each topic's data socket, so NAT hole-punching
    // (and anything else that needs to reach a specific peer) fires from
    // the SAME socket that topic's traffic actually uses -- required once
    // set_multiplex(false) gives each topic its own port/NAT mapping,
    // since punching from the wrong socket opens the wrong mapping and
    // the peer's simultaneous punch back never gets through.
    std::unordered_map<uint32_t, UdpTransport*> topic_route_;

    void nat_punch(const PeerAddr& peer, UdpTransport& via) {
        for (int i = 0; i < 3; ++i) {
            via.publish_raw(kNatPunchTopicId, nullptr, 0, peer);
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
    }

    bool repunch_enabled_ = false;
    std::chrono::duration<double> repunch_interval_{5.0};
    std::atomic<bool> repunch_stop_{false};
    std::thread repunch_thread_;

    bool relay_enabled_ = false;
    uint32_t relay_ip_ = 0;
    uint16_t relay_port_ = kRelayDefaultPort;
    std::atomic<bool> relay_stop_{false};
    std::thread relay_keepalive_thread_;

    PeerAddr relay_peer() const { return PeerAddr{relay_ip_, relay_port_}; }

    // Registers (and, via the keepalive thread, keeps registered) every
    // topic-owning transport with the relay -- one REGISTER packet per
    // (transport, topic) pair, sent from that exact transport's socket
    // so the relay's forwarded copies land on the same socket that
    // topic's direct traffic already listens on. (Dedup is armed
    // earlier, in ensure_started(), before transport_.start() -- see
    // that call site for why the ordering matters.)
    void register_all_topics_with_relay(const std::vector<std::pair<UdpTransport*, std::vector<uint32_t>>>& groups) {
        uint8_t buf[8];
        for (const auto& g : groups) {
            for (uint32_t topic : g.second) {
                size_t len = encode_relay_register(topic, buf, sizeof(buf));
                struct sockaddr_in dest{};
                dest.sin_family = AF_INET;
                dest.sin_addr.s_addr = htonl(relay_ip_);
                dest.sin_port = htons(relay_port_);
                ::sendto(g.first->native_handle(), buf, len, 0,
                         reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest));
            }
        }
    }

    void start_relay_keepalive_thread(std::vector<std::pair<UdpTransport*, std::vector<uint32_t>>> groups) {
        relay_keepalive_thread_ = std::thread([this, groups = std::move(groups)] {
            // Must outpace the relay daemon's kMemberTtlSeconds (30s) by
            // a comfortable margin so a scheduling hiccup doesn't drop
            // this node out of a topic's forwarding group.
            while (!relay_stop_.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(10));
                if (relay_stop_.load()) break;
                register_all_topics_with_relay(groups);
            }
        });
    }

    void start_repunch_thread() {
        repunch_thread_ = std::thread([this] {
            while (!repunch_stop_.load()) {
                std::this_thread::sleep_for(
                    std::chrono::duration_cast<std::chrono::milliseconds>(repunch_interval_));
                if (repunch_stop_.load()) break;
                // Snapshot (topic, peer, transport) triples under the lock,
                // then punch outside it -- nat_punch() sleeps between
                // packets, and holding state_mutex_ across those sleeps
                // would stall any advertise/subscribe/publish call racing
                // to acquire it on another thread.
                std::vector<std::tuple<UdpTransport*, PeerAddr>> targets;
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    for (const auto& kv : peers_) {
                        uint32_t topic = kv.first;
                        auto it = topic_route_.find(topic);
                        UdpTransport* via = (it != topic_route_.end()) ? it->second : &transport_;
                        for (const auto& p : kv.second) targets.emplace_back(via, p);
                    }
                }
                for (auto& t : targets) {
                    if (repunch_stop_.load()) break;
                    nat_punch(std::get<1>(t), *std::get<0>(t));
                }
            }
        });
    }
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

// Standard message types (relink/standard_msgs.hpp) at ROS-familiar,
// top-level namespace names -- `std_msgs::Header`, `geometry_msgs::Pose`,
// `sensor_msgs::Imu`, `nav_msgs::Odometry`, etc., not nested under
// `relink::`, matching how ROS/NoROSLib code addresses them.
namespace std_msgs = relink::std_msgs;
namespace geometry_msgs = relink::geometry_msgs;
namespace sensor_msgs = relink::sensor_msgs;
namespace nav_msgs = relink::nav_msgs;
namespace diagnostic_msgs = relink::diagnostic_msgs;
namespace trajectory_msgs = relink::trajectory_msgs;
namespace actionlib_msgs = relink::actionlib_msgs;

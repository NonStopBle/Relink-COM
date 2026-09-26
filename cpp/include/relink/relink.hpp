// Step 6: the public ReLink API -- RelinkNode, wiring together the ring
// buffer (step 2), UDP data thread (step 3), rlcore client (step 4),
// and multicast discovery (step 5) behind the templated
// advertise/subscribe/publish<T> surface shown in relink_example.cpp.

#pragma once

#include "relink/wire.hpp"
#include "relink/udp_transport.hpp"
#include "relink/rlcore_client.hpp"
#include "relink/crypto.hpp"
#include "relink/multicast_discovery.hpp"
#include "relink/image.hpp"
#include "relink/compressed_image.hpp"
#include "relink/topic_hash.hpp"
#include "relink/topic_directory.hpp"
#include "relink/standard_msgs.hpp"
#include "relink/relay_wire.hpp"
#include "relink/shm_transport.hpp"
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

        // Sets the pre-shared AES-256 key (as 64 hex characters, e.g.
        // the output of `relink-rlcore --generate-key`) used to encrypt
        // this node's RegisterRequest/RegisterAck exchange with rlcore.
        // Throws if `hex_key` isn't exactly 64 valid hex characters,
        // rather than silently registering unencrypted -- a typo'd key
        // here should never look like a working, plaintext setup.
        // rlcore must be started with the SAME key via --encrypt-key
        // for registration to succeed; a mismatched or missing key on
        // either side makes every RegisterRequest/RegisterAck fail to
        // decrypt and get dropped as malformed (see rlcore_client.hpp).
        void setEncryptKey(const std::string& hex_key) {
            if (!hex_to_key32(hex_key, encrypt_key_)) {
                throw std::runtime_error(
                    "set_rlcore.setEncryptKey: expected 64 hex characters (a 32-byte AES-256 "
                    "key) -- generate one with `relink-rlcore --generate-key`");
            }
            has_encrypt_key_ = true;
        }

        bool ip_is_set() const { return ip_set_; }
        uint32_t resolved_ip() const { return rlcore_ip_; }
        uint16_t resolved_port() const { return rlcore_port_; }
        bool has_encrypt_key() const { return has_encrypt_key_; }
        const uint8_t* encrypt_key() const { return has_encrypt_key_ ? encrypt_key_ : nullptr; }

        // Opt-in shortcut for the common case: rlcore itself was started
        // with --relay (or --nat, which implies it), which folds relay
        // data forwarding into this SAME ip:port instead of a separate
        // standalone relink-relay process (see rlcore's --relay comment
        // in relink_rlcore.cpp). Equivalent to calling the node's own
        // set_relay(ip, port) with this config's own ip()/port(), so
        // there's no second address to keep in sync with the first.
        // Resolved against ip()/port() at ensure_started() time (first
        // spin()/publish()/subscribe call), so call order relative to
        // ip()/port() doesn't matter. Use RelinkNode::set_relay(ip, port)
        // directly instead only when relaying through a DIFFERENT address
        // than rlcore itself (e.g. the standalone relink-relay binary on
        // its own default port).
        void setRelay(bool enabled = true) { relay_requested_ = enabled; }
        bool relay_requested() const { return relay_requested_; }

    private:
        RelinkNode& owner_;
        bool ip_set_ = false;
        uint32_t rlcore_ip_ = 0;
        uint16_t rlcore_port_ = kRlCoreDefaultPort;
        bool has_encrypt_key_ = false;
        uint8_t encrypt_key_[kAesKeyBytes] = {};
        bool relay_requested_ = false;
    };

    RelinkNode() : set_rlcore(*this) {}

    RlCoreConfig set_rlcore;

    // --- mode B, mutually exclusive with mode A (per spec) ---
    void use_multicast_discovery() {
        select_mode(DiscoveryMode::Multicast);
    }

    // ROS_DOMAIN_ID-style isolation for Mode B only (Mode A/rlcore
    // already isolates deployments via each rlcore daemon's own
    // ip:port). Nodes with different network_id join DIFFERENT
    // multicast group addresses (see derive_multicast_group() in
    // multicast_discovery.hpp) -- true OS-level isolation, not a
    // payload check, so two unrelated deployments sharing a LAN never
    // even receive each other's beacons. Default 0 maps to today's
    // fixed multicast address, so existing single-domain deployments
    // that never call this see zero behavior change. Must be called
    // before use_multicast_discovery()/first traffic -- same ordering
    // requirement as advertise/subscribe before ensure_started().
    void set_network_id(uint16_t network_id) { network_id_ = network_id; }

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
        rlcore_reregister_stop_ = true;
        if (rlcore_reregister_thread_.joinable()) rlcore_reregister_thread_.join();
        relay_stop_ = true;
        if (relay_keepalive_thread_.joinable()) relay_keepalive_thread_.join();
        if (initial_punch_thread_.joinable()) initial_punch_thread_.join();
        {
            std::lock_guard<std::mutex> lock(punch_threads_mutex_);
            for (auto& t : punch_threads_) if (t.joinable()) t.join();
        }
        stop_requested_ = true;
        if (shm_poll_thread_.joinable()) shm_poll_thread_.join();
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

    // Whether a relay path (via set_relay() or set_rlcore.setRelay())
    // is active. Callers that inspect peers_for_topic()/publish_raw()'s
    // return value to decide "is anyone listening" should check this
    // first -- with relay enabled, publish_raw() always attempts
    // delivery via the relay's fixed address regardless of whether any
    // DIRECT peer has been learned yet, so an empty peers_for_topic()
    // list no longer means "no-op publish" the way it does without relay.
    bool relay_active() const { return relay_enabled_; }

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

    // --- advertise: publisher-side topic declaration. pair/pair_id are
    // a purely LOCAL (this node only) hint under set_multiplex(false):
    // any topics advertised/subscribed here with pair=true and the same
    // pair_id share one UDP port instead of each getting its own -- see
    // transport_for()/register_pair(). Ignored entirely when multiplex
    // is on (everything already shares transport_). No wire-format
    // involvement and no requirement that a peer's pub or sub side make
    // the same pairing choice -- discovery already resolves peers by
    // topic_id regardless of which port a topic happens to live on. ---
    template <typename T>
    void advertise(const std::string& name, bool secure = false, bool checksum = false,
                   bool pair = false, uint32_t pair_id = 0) {
        advertise<T>(topic_id_for(name), secure, checksum, pair, pair_id);
    }

    template <typename T>
    void advertise(uint32_t topic_id, bool /*secure*/ = false, bool /*checksum*/ = false,
                   bool pair = false, uint32_t pair_id = 0) {
        static_assert(std::is_trivially_copyable<T>::value,
                      "advertise<T>: T must be trivially copyable");
        if constexpr (std::is_same<T, ImageChunk>::value || std::is_same<T, CompressedImageChunk>::value) {
            transport_.enable_large_buffers(); // Image/CompressedImage always stay on the shared transport
        } else {
            register_pair(topic_id, pair, pair_id);
            transport_for(topic_id);
        }
        std::lock_guard<std::mutex> lock(state_mutex_);
        declared_topics_.insert(topic_id);
        advertised_topics_.insert(topic_id);
    }

    // --- subscribe: receiver-side topic declaration + typed callback,
    // invoked inline on the data thread per spec's v1 threading design.
    // pair/pair_id: see advertise<T>() above. ---
    template <typename T, typename Callback>
    void subscribe(const std::string& name, Callback callback, bool secure = false,
                   bool pair = false, uint32_t pair_id = 0) {
        subscribe<T>(topic_id_for(name), std::move(callback), secure, pair, pair_id);
    }

    template <typename T, typename Callback>
    void subscribe(uint32_t topic_id, Callback callback, bool /*secure*/ = false,
                   bool pair = false, uint32_t pair_id = 0) {
        static_assert(std::is_trivially_copyable<T>::value,
                      "subscribe<T>: T must be trivially copyable");
        constexpr bool kIsLargeBlob = std::is_same<T, ImageChunk>::value || std::is_same<T, CompressedImageChunk>::value;
        if constexpr (!kIsLargeBlob) register_pair(topic_id, pair, pair_id);
        UdpTransport& t = kIsLargeBlob ? transport_ : transport_for(topic_id);
        if constexpr (kIsLargeBlob) {
            transport_.enable_large_buffers();
        }
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            declared_topics_.insert(topic_id);
            subscribed_topics_.insert(topic_id);
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

    void advertise_raw(const std::string& name, bool pair = false, uint32_t pair_id = 0) {
        advertise_raw(topic_id_for(name), pair, pair_id);
    }
    void advertise_raw(uint32_t topic_id, bool pair = false, uint32_t pair_id = 0) {
        register_pair(topic_id, pair, pair_id);
        transport_for(topic_id);
        std::lock_guard<std::mutex> lock(state_mutex_);
        declared_topics_.insert(topic_id);
        advertised_topics_.insert(topic_id);
    }

    void subscribe_raw(const std::string& name, RawCallback callback, bool pair = false, uint32_t pair_id = 0) {
        subscribe_raw(topic_id_for(name), std::move(callback), pair, pair_id);
    }
    void subscribe_raw(uint32_t topic_id, RawCallback callback, bool pair = false, uint32_t pair_id = 0) {
        register_pair(topic_id, pair, pair_id);
        UdpTransport& t = transport_for(topic_id);
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            declared_topics_.insert(topic_id);
            subscribed_topics_.insert(topic_id);
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
            advertised_topics_.insert(topic_id);
            auto it = peers_.find(topic_id);
            if (it != peers_.end()) peers = it->second;
        }
        UdpTransport& t = transport_for(topic_id);
        uint16_t seq = t.next_seq(); // shared across every direct peer AND the relay copy
        bool all_ok = true;
        for (const auto& peer : peers) {
            all_ok = t.publish_raw(topic_id, payload, payload_len, peer, seq) && all_ok;
        }
        bool relay_ok = false;
        if (relay_enabled_) relay_ok = t.publish_raw(topic_id, payload, payload_len, relay_peer(), seq);
        // Direct delivery counts as success only if there was at least
        // one direct peer to send to; the relay path counts as success
        // on its own -- it doesn't need a direct peer address at all, so
        // an empty `peers` list under relay is not a failure.
        return (all_ok && !peers.empty()) || relay_ok;
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
            // Mirrors publish_raw()'s bookkeeping (this method never had
            // it) -- harmless no-op for rlcore registration if this is
            // the first call for this topic (ensure_started() already
            // snapshotted declared_topics_ by now; advertise<T>() before
            // the first spin_once()/publish() is still required for
            // actual peer discovery), but keeps advertised_topics_
            // accurate for rl_topic info's role reporting either way.
            declared_topics_.insert(topic_id);
            advertised_topics_.insert(topic_id);
            auto it = peers_.find(topic_id);
            if (it != peers_.end()) peers = it->second;
        }
        constexpr bool kIsLargeBlob = std::is_same<T, ImageChunk>::value || std::is_same<T, CompressedImageChunk>::value;
        UdpTransport& t = kIsLargeBlob ? transport_ : transport_for(topic_id);
        uint16_t seq = t.next_seq(); // shared across every direct peer AND the relay copy
        bool all_ok = true;
        for (const auto& peer : peers) {
            all_ok = t.publish_raw(topic_id, &value, sizeof(T), peer, seq) && all_ok;
        }
        bool relay_ok = false;
        if (relay_enabled_ && !kIsLargeBlob) {
            relay_ok = t.publish_raw(topic_id, &value, sizeof(T), relay_peer(), seq);
        }
        return (all_ok && !peers.empty()) || relay_ok;
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
            subscribed_topics_.insert(topic_id);
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

    // --- CompressedImage: same MTU-chunking as Image, plus a capture
    // timestamp and the encoder quality used, carried on every chunk so
    // a receiver can measure end-to-end latency and report the quality
    // actually used, with no side channel back to the publisher. Pair
    // this with AdaptiveBitrateController (see adaptive_bitrate.hpp) to
    // pick `quality` per frame -- this method itself does no encoding
    // and no quality selection, same "bring your own codec" stance as
    // Image. See compressed_image.hpp and examples/cpp/camera_stream.cpp. ---

    void advertise_compressed_image(const std::string& name) { advertise_compressed_image(topic_id_for(name)); }
    void advertise_compressed_image(uint32_t topic_id) { advertise<CompressedImageChunk>(topic_id); }

    // data/len is already-compressed bytes (e.g. a JPEG buffer).
    // capture_timestamp_ns defaults to now() -- override it if you
    // captured the frame earlier than this call (e.g. batching).
    // quality is purely informational: the encoder quality (0-100) you
    // used for this frame, so a subscriber can display/log it without
    // a return channel. Returns false if `len` exceeds
    // kMaxCompressedImageBytes, same "never silently truncate" rule as
    // publish_image().
    bool publish_compressed_image(const std::string& name, const uint8_t* data, size_t len,
                                   uint8_t quality = 0, uint32_t frame_id = kAutoFrameId,
                                   uint64_t capture_timestamp_ns = 0) {
        return publish_compressed_image(topic_id_for(name), data, len, quality, frame_id, capture_timestamp_ns);
    }

    bool publish_compressed_image(uint32_t topic_id, const uint8_t* data, size_t len,
                                   uint8_t quality = 0, uint32_t frame_id = kAutoFrameId,
                                   uint64_t capture_timestamp_ns = 0) {
        if (len > kMaxCompressedImageBytes) return false;
        if (frame_id == kAutoFrameId) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            frame_id = next_compressed_image_frame_id_[topic_id]++;
        }
        if (capture_timestamp_ns == 0) {
            capture_timestamp_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
        }
        ensure_started();

        std::vector<PeerAddr> peers;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            auto it = peers_.find(topic_id);
            if (it != peers_.end()) peers = it->second;
        }

        uint16_t chunk_count = static_cast<uint16_t>(
            (len + kCompressedImageChunkDataBytes - 1) / kCompressedImageChunkDataBytes);
        if (chunk_count == 0) chunk_count = 1; // empty image is still one chunk

        bool all_ok = true;
        for (uint16_t i = 0; i < chunk_count; ++i) {
#pragma pack(push, 1)
            struct ChunkHeader {
                uint32_t frame_id;
                uint16_t chunk_index;
                uint16_t chunk_count;
                uint16_t chunk_bytes;
                uint64_t capture_timestamp_ns;
                uint8_t  quality;
            };
#pragma pack(pop)
            static_assert(sizeof(ChunkHeader) == kCompressedImageChunkHeaderBytes,
                          "ChunkHeader must exactly match the wire chunk header layout");
            ChunkHeader chdr{frame_id, i, chunk_count, 0, capture_timestamp_ns, quality};
            size_t offset = size_t(i) * kCompressedImageChunkDataBytes;
            size_t n = std::min(kCompressedImageChunkDataBytes, len - offset);
            chdr.chunk_bytes = static_cast<uint16_t>(n);

            for (const auto& peer : peers) {
                all_ok = transport_.publish_scattered(topic_id, &chdr, sizeof(chdr),
                                                        data + offset, n, peer) && all_ok;
            }
        }
        return all_ok;
    }

    // Subscribes to a topic of CompressedImage chunks; `callback`
    // (frame_id, data, capture_timestamp_ns, quality) fires once per
    // COMPLETE image. Same drop-on-incomplete-frame tradeoff, same
    // "slow callback blocks the data thread" warning, as
    // subscribe_image() -- see there for the full explanation.
    template <typename Callback>
    void subscribe_compressed_image(const std::string& name, Callback callback) {
        subscribe_compressed_image(topic_id_for(name), std::move(callback));
    }

    template <typename Callback>
    void subscribe_compressed_image(uint32_t topic_id, Callback callback) {
        transport_.enable_large_buffers();
        auto reassembler = std::make_shared<CompressedImageReassembler>(
            [callback](uint32_t frame_id, const std::vector<uint8_t>& image,
                       uint64_t capture_timestamp_ns, uint8_t quality) {
                callback(frame_id, image, capture_timestamp_ns, quality);
            });
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            compressed_image_reassemblers_.push_back(reassembler); // keep alive for node lifetime
            declared_topics_.insert(topic_id);
            subscribed_topics_.insert(topic_id);
        }
        transport_.set_topic_handler(topic_id, [reassembler](const uint8_t* payload, size_t len) {
            if (len < kCompressedImageChunkHeaderBytes) return;
            uint32_t frame_id; uint16_t chunk_index, chunk_count, chunk_bytes;
            uint64_t capture_timestamp_ns; uint8_t quality;
            std::memcpy(&frame_id, payload, sizeof(frame_id));
            std::memcpy(&chunk_index, payload + 4, sizeof(chunk_index));
            std::memcpy(&chunk_count, payload + 6, sizeof(chunk_count));
            std::memcpy(&chunk_bytes, payload + 8, sizeof(chunk_bytes));
            std::memcpy(&capture_timestamp_ns, payload + 10, sizeof(capture_timestamp_ns));
            std::memcpy(&quality, payload + 18, sizeof(quality));
            const uint8_t* data = payload + kCompressedImageChunkHeaderBytes;
            size_t data_len = len - kCompressedImageChunkHeaderBytes;
            if (chunk_bytes != data_len) return; // mismatch: drop, never misinterpret bytes
            reassembler->on_chunk_raw(frame_id, chunk_index, chunk_count, data, chunk_bytes,
                                       capture_timestamp_ns, quality);
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

    void request_stop() {
        stop_requested_ = true;
        std::lock_guard<std::mutex> lock(shm_mutex_);
        for (auto& kv : shm_rings_) {
            if (kv.second->is_creator()) kv.second->unlink();
        }
    }

    // --- Same-host IPC opt-in: explicit shared-memory fast path for two
    // processes known to be on the same machine. NOT auto-selected via
    // discovery yet (see the phased local-IPC plan) -- both sides must
    // call the matching *_local_ipc(same topic) themselves, with the
    // SAME capacity/max_payload (mismatched values fail to attach --
    // see ShmRing::open()'s compatibility check).
    //
    // max_payload defaults to relink::kShmMaxPayload (1400, matching
    // UDP's MTU-driven MAX_PAYLOAD_BYTES) for small messages. Pass a
    // larger value for whole Image/CompressedImage frames -- shared
    // memory has no MTU, so unlike the UDP path, a frame goes over in
    // ONE slot, no chunking/reassembly needed (see shm_transport.hpp's
    // header comment). A raw 1920x1080x3 frame is ~6.2MB; size
    // max_payload (and capacity) to your actual resolution/backlog
    // needs -- the segment reserves capacity*max_payload bytes up
    // front, unlike UDP which reserves nothing.

    bool advertise_local_ipc(uint32_t topic_id, uint32_t capacity = relink::kShmDefaultCapacity,
                              uint32_t max_payload = relink::kShmMaxPayload) {
        std::lock_guard<std::mutex> lock(shm_mutex_);
        if (shm_rings_.count(topic_id)) return true;
        auto ring = std::make_unique<relink::ShmRing>();
        if (!ring->open("/relink_topic_" + std::to_string(topic_id), capacity, max_payload)) return false;
        shm_rings_.emplace(topic_id, std::move(ring));
        declared_topics_.insert(topic_id);
        return true;
    }
    bool advertise_local_ipc(const std::string& name, uint32_t capacity = relink::kShmDefaultCapacity,
                              uint32_t max_payload = relink::kShmMaxPayload) {
        return advertise_local_ipc(topic_id_for(name), capacity, max_payload);
    }

    // `payload` points directly into shared memory (zero-copy -- see
    // shm_transport.hpp's try_pop_zero_copy()) and is valid ONLY for the
    // duration of this call; copy out anything you need to keep past
    // it. Same discipline as UdpTransport's own raw callback, which
    // already hands a pointer into a reused receive buffer under the
    // identical constraint -- not a new contract for this codebase.
    using ShmCallback = std::function<void(const uint8_t* payload, size_t len)>;

    bool subscribe_local_ipc(uint32_t topic_id, ShmCallback callback,
                              uint32_t capacity = relink::kShmDefaultCapacity,
                              uint32_t max_payload = relink::kShmMaxPayload) {
        if (!advertise_local_ipc(topic_id, capacity, max_payload)) return false;
        {
            std::lock_guard<std::mutex> lock(shm_mutex_);
            shm_handlers_[topic_id] = std::move(callback);
        }
        ensure_shm_poll_thread();
        return true;
    }
    bool subscribe_local_ipc(const std::string& name, ShmCallback callback,
                              uint32_t capacity = relink::kShmDefaultCapacity,
                              uint32_t max_payload = relink::kShmMaxPayload) {
        return subscribe_local_ipc(topic_id_for(name), std::move(callback), capacity, max_payload);
    }

    // Convenience wrappers for whole Image/CompressedImage frames over
    // local IPC -- same mechanism as advertise_local_ipc above, just
    // defaulting max_payload to a full 1920x1080 BGR8 frame's size so
    // callers don't have to compute it themselves. Use a smaller
    // max_payload directly via advertise_local_ipc() for a known
    // smaller resolution or compressed (JPEG) frames.
    static constexpr uint32_t kShmImageDefaultMaxPayload = 1920u * 1080u * 3u;  // ~6.2MB, raw BGR8 1080p

    bool advertise_local_ipc_image(uint32_t topic_id, uint32_t max_payload = kShmImageDefaultMaxPayload,
                                    uint32_t capacity = 8) {
        return advertise_local_ipc(topic_id, capacity, max_payload);
    }
    bool subscribe_local_ipc_image(uint32_t topic_id, ShmCallback callback,
                                    uint32_t max_payload = kShmImageDefaultMaxPayload,
                                    uint32_t capacity = 8) {
        return subscribe_local_ipc(topic_id, std::move(callback), capacity, max_payload);
    }

    bool publish_local_ipc(uint32_t topic_id, const void* payload, size_t len) {
        relink::ShmRing* ring = nullptr;
        uint32_t seq = 0;
        {
            // seq must be drawn under the same lock as the ring lookup,
            // not after releasing it: shm_seq_[topic_id]++ inserts a new
            // entry (a structural mutation of the unordered_map) on this
            // topic's first publish, same as shm_rings_.find() would if
            // it weren't already guarded. Two threads publishing to two
            // different topic_ids on the same node for the first time at
            // once would otherwise race on that insert -- unlike bumping
            // an EXISTING entry, which is safe across different keys,
            // that's undefined behavior. try_push() itself is left
            // outside the lock: it's the single-producer write into
            // already-owned shared memory (per-topic ShmRing is SPSC, a
            // separate documented caller contract), not node bookkeeping,
            // so serializing it here would only add unneeded contention
            // across unrelated topics.
            std::lock_guard<std::mutex> lock(shm_mutex_);
            auto it = shm_rings_.find(topic_id);
            if (it == shm_rings_.end()) return false;
            ring = it->second.get();
            seq = shm_seq_[topic_id]++;
        }
        return ring->try_push(static_cast<const uint8_t*>(payload), static_cast<uint32_t>(len), seq);
    }
    bool publish_local_ipc(const std::string& name, const void* payload, size_t len) {
        return publish_local_ipc(topic_id_for(name), payload, len);
    }

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

    // PIDs of same-host, shm-capable peers discovered for `topic_id` so
    // far (see multicast_discovery.hpp's beacon extension). A signal
    // only: does not itself start using local IPC for this topic --
    // call advertise_local_ipc/subscribe_local_ipc/publish_local_ipc
    // yourself once you've decided to. Empty if discovery hasn't found
    // one yet, or isn't in use (rlcore mode doesn't populate this).
    std::vector<uint32_t> local_ipc_peers(uint32_t topic_id) {
        std::lock_guard<std::mutex> lock(shm_mutex_);
        auto it = local_ipc_candidates_.find(topic_id);
        if (it == local_ipc_candidates_.end()) return {};
        return std::vector<uint32_t>(it->second.begin(), it->second.end());
    }
    std::vector<uint32_t> local_ipc_peers(const std::string& name) {
        return local_ipc_peers(topic_id_for(name));
    }

private:
    friend class RlCoreConfig;

    // One thread services every local-IPC topic on this node, same
    // "one dedicated data path per node" shape as UdpTransport's own
    // receive thread -- polling instead of blocking since the ring has
    // no OS-level wakeup primitive (see shm_transport.hpp's design
    // notes on why: avoiding named-semaphore bindings for v1).
    void ensure_shm_poll_thread() {
        bool expected = false;
        if (!shm_poll_started_.compare_exchange_strong(expected, true)) return;
        shm_poll_thread_ = std::thread([this]() {
            // Zero-copy: try_pop_zero_copy() hands the callback a
            // pointer directly into the shared-memory slot (valid only
            // for the duration of the call -- head only advances after
            // it returns), so there's no intermediate buffer to size or
            // copy into here at all. Matters most for Image/
            // CompressedImage-sized rings, where an extra memcpy would
            // otherwise undo a real fraction of the point of using
            // shared memory.
            while (!stop_requested_.load()) {
                bool delivered_any = false;
                std::vector<std::pair<uint32_t, relink::ShmRing*>> rings;
                {
                    std::lock_guard<std::mutex> lock(shm_mutex_);
                    for (auto& kv : shm_handlers_) {
                        auto it = shm_rings_.find(kv.first);
                        if (it != shm_rings_.end()) rings.emplace_back(kv.first, it->second.get());
                    }
                }
                for (auto& tr : rings) {
                    ShmCallback cb;
                    {
                        std::lock_guard<std::mutex> lock(shm_mutex_);
                        auto it = shm_handlers_.find(tr.first);
                        if (it != shm_handlers_.end()) cb = it->second;
                    }
                    if (!cb) continue;
                    bool popped = tr.second->try_pop_zero_copy(
                        [&](const uint8_t* payload, uint32_t len, uint32_t /*seq*/) {
                            cb(payload, len);
                        });
                    if (popped) delivered_any = true;
                }
                if (!delivered_any) std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        });
        shm_thread_owned_ = true;
    }

    std::mutex shm_mutex_;
    std::unordered_map<uint32_t, std::unique_ptr<relink::ShmRing>> shm_rings_;
    std::unordered_map<uint32_t, ShmCallback> shm_handlers_;
    std::unordered_map<uint32_t, uint32_t> shm_seq_;
    std::unordered_map<uint32_t, std::unordered_set<uint32_t>> local_ipc_candidates_;
    std::atomic<bool> shm_poll_started_{false};
    bool shm_thread_owned_ = false;
    std::thread shm_poll_thread_;

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
        uint64_t key = group_key_locked(topic_id);
        auto it = topic_transports_.find(key);
        if (it != topic_transports_.end()) return *it->second;
        auto t = std::make_unique<UdpTransport>();
        t->bind(0);
        UdpTransport* raw = t.get();
        topic_transports_.emplace(key, std::move(t));
        return *raw;
    }

    // Caller must hold state_mutex_.
    uint64_t group_key_locked(uint32_t topic_id) const {
        auto pit = topic_pair_id_.find(topic_id);
        if (pit != topic_pair_id_.end()) return kPairKeyTag | pit->second;
        return topic_id;
    }

    // Records that `topic_id` should share a port with every other topic
    // registered under the same pair_id, purely a local (this node only)
    // port-allocation decision -- no wire-format involvement, see
    // group_key_locked()/transport_for(). Must be called BEFORE
    // transport_for(topic_id) so the topic's transport is created (or
    // found) under the right group key from the start. A topic_id
    // re-declared later with a different pair_id (or paired then later
    // unpaired) is almost certainly a bug -- fail loudly rather than
    // silently rebinding it to a new port out from under a caller who
    // may already be relying on the earlier port.
    void register_pair(uint32_t topic_id, bool pair, uint32_t pair_id) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = topic_pair_id_.find(topic_id);
        if (!pair) {
            if (it != topic_pair_id_.end()) {
                throw std::runtime_error(
                    "relink: topic " + std::to_string(topic_id) +
                    " already paired under pair_id " + std::to_string(it->second) +
                    ", cannot unpair it later");
            }
            return;
        }
        if (it != topic_pair_id_.end() && it->second != pair_id) {
            throw std::runtime_error(
                "relink: topic " + std::to_string(topic_id) +
                " already paired under pair_id " + std::to_string(it->second) +
                ", cannot re-pair under pair_id " + std::to_string(pair_id));
        }
        topic_pair_id_[topic_id] = pair_id;
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
        if (set_rlcore.relay_requested() && !relay_enabled_) {
            if (mode != DiscoveryMode::RlCore) {
                throw std::runtime_error(
                    "set_rlcore.setRelay(true) requires rlcore discovery mode -- "
                    "call set_rlcore.ip(...) first");
            }
            relay_enabled_ = true;
            relay_ip_ = set_rlcore.resolved_ip();
            relay_port_ = set_rlcore.resolved_port();
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
                // Bucket by transport POINTER, not by iterating
                // topic_transports_'s own entries directly: a paired
                // group of topics all resolves to the SAME map entry
                // (same group key, see group_key_locked()), so the old
                // "one map entry == one topic" assumption doesn't hold
                // once pairing is in use -- recompute each declared
                // topic's group key here and group by the transport it
                // resolves to, so a paired topic's peers all end up
                // correctly listed together in one registration/beacon.
                std::unordered_map<UdpTransport*, std::vector<uint32_t>> by_transport;
                std::unordered_set<uint32_t> demuxed;
                for (uint32_t id : declared_topics_) {
                    auto it = topic_transports_.find(group_key_locked(id));
                    if (it == topic_transports_.end()) continue;
                    by_transport[it->second.get()].push_back(id);
                    demuxed.insert(id);
                }
                for (auto& kv : by_transport) groups.push_back(TopicGroup{kv.first, kv.second});
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
            // Also needed under plain RlCore mode (no --relay) now that a
            // peer can carry a second, same-NAT LAN candidate address
            // (see the RegisterAckPeer handling below): if that peer's
            // NAT/router happens to support hairpinning after all, both
            // the LAN and public copies of a message can legitimately
            // arrive, and this same seq_num-based check is exactly what
            // collapses them back into one delivery.
            if (relay_enabled_ || mode == DiscoveryMode::RlCore) {
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
                // Only ONE quick attempt here (not the 3-retry/
                // exponential-backoff default, which can block
                // ensure_started() for ~3.5s if rlcore happens to be
                // briefly unreachable) -- the periodic re-registration
                // thread started below retries every few seconds for
                // the rest of the node's lifetime, so a slow/late rlcore
                // is recovered from in the background instead of
                // stalling startup.
                auto outcome = register_with_rlcore_on_socket(
                    g.transport->native_handle(),
                    set_rlcore.resolved_ip(), set_rlcore.resolved_port(),
                    self_ip, g.transport->local_port(),
                    g.topics.data(), static_cast<uint16_t>(g.topics.size()),
                    /*max_retries=*/1, /*timeout_ms=*/300, g.transport,
                    set_rlcore.encrypt_key());
                if (outcome.ok) {
                    std::lock_guard<std::mutex> lock2(state_mutex_);
                    for (const auto& p : outcome.peers) {
                        // Copy fields out of RegisterAckPeer (#pragma
                        // pack(1) in wire.hpp) into plain locals before
                        // using them -- p.topic_id lives at a non-4-byte-
                        // aligned offset in the packed struct, and
                        // passing it by reference straight into
                        // unordered_map::operator[](const key_type&)
                        // binds a reference to that misaligned address.
                        // That's UB, and at -O2 it isn't just theoretical:
                        // it actually crashed with a general protection
                        // fault under real traffic (many peers in one
                        // vector, e.g. the ~1000-topic large-scale test).
                        uint32_t ip = p.ip;
                        uint16_t port = p.port;
                        uint32_t topic_id = p.topic_id;
                        uint32_t lan_ip = p.lan_ip;
                        uint16_t lan_port = p.lan_port;
                        PeerAddr addr{ip, port};
                        peers_[topic_id].push_back(addr);
                        newly_learned_peers.push_back({g.transport, addr});
                        // Same-NAT ("hairpin") fallback: also add this
                        // peer's self-reported LAN address as a SECOND
                        // destination for this topic, not a replacement --
                        // publish() already sends the same seq_num to
                        // every entry in peers_[topic_id], and the
                        // enable_relay_dedup() call above collapses
                        // whichever copy(ies) actually arrive back into
                        // one delivery. See RegisterAckPeer's doc comment
                        // in wire.hpp. Skipped when rlcore had no LAN
                        // candidate for this peer (lan_ip == 0) or it's
                        // identical to the primary address already added.
                        if (lan_ip != 0 && !(lan_ip == ip && lan_port == port)) {
                            PeerAddr lan_addr{lan_ip, lan_port};
                            peers_[topic_id].push_back(lan_addr);
                            newly_learned_peers.push_back({g.transport, lan_addr});
                        }
                    }
                }
            }
            rlcore_self_ip_ = self_ip;
            rlcore_groups_.clear();
            for (const auto& g : groups) rlcore_groups_.emplace_back(g.transport, g.topics);
            // Tell rlcore about any names we resolved for these topics --
            // rl_topic list/info's Name field depends on this (a separate
            // wire protocol from registration/data traffic, so it can be
            // missing even though everything else works). This first send
            // is fire-and-forget same as before, but no longer the ONLY
            // attempt: start_rlcore_reregister_thread() below repeats it
            // every tick forever, same as it already does for the role
            // announce, so one lost packet (plausible for the very first
            // datagram a fresh socket sends across a real WAN path) no
            // longer permanently hides the name from rl_topic.
            announce_topic_names_to_rlcore();

            // If registration failed after retries, register_with_rlcore
            // already logged an error; proceed with an empty peer table
            // rather than crashing the node (spec: never hang forever).
        } else {
            // Multicast: self_ip auto-detected via the multicast group
            // address as the "peer" for route selection. group_ip AND
            // group_port are BOTH derived from network_id_ (see
            // set_network_id()) -- the default network_id=0 reproduces
            // today's fixed address/port exactly. Port must vary too,
            // not just the address -- see derive_multicast_port()'s
            // comment for the SO_REUSEPORT reason why address alone
            // does not isolate two network_ids sharing a host.
            MulticastDiscoveryConfig cfg;
            cfg.group_ip = derive_multicast_group(network_id_);
            cfg.group_port = derive_multicast_port(network_id_);
            cfg.self_ip = detect_local_ip_for_peer(
                ipv4_to_host_order(cfg.group_ip), cfg.group_port);
            cfg.self_pid = static_cast<uint32_t>(getpid());
            cfg.shm_capable = true;  // this build has shm_transport.hpp
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
                if (p.shm_capable) {
                    std::lock_guard<std::mutex> lock3(shm_mutex_);
                    local_ipc_candidates_[topic].insert(p.pid);
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
        //
        // Backgrounded, not run inline here: this whole ensure_started()
        // call runs SYNCHRONOUSLY on the caller's own thread (the first
        // publish()/spin_once() call). nat_punch() sleeps 30ms between
        // each of 3 packets per peer, so at a handful of peers that's
        // invisible, but at real large-system peer counts (e.g.
        // ~500-1000, one per topic) it's peer_count * 3 * 30ms of
        // blocking sleep on the caller's own thread -- measured ~45s for
        // 500 peers, which starved the caller's entire publish loop for
        // the whole duration of a test before it ever got to send a
        // second message. Punching a moment later in the background
        // costs nothing real (the reregister loop already punches
        // newly-discovered peers the same asynchronous way -- see
        // start_rlcore_reregister_thread()), so there's no reason for
        // this one-time burst to block startup at all.
        if (!newly_learned_peers.empty()) {
            // A joinable member thread (joined in ~RelinkNode()), not
            // detach(): a detached thread capturing `this` could still
            // be running nat_punch() -> a member function call -- after
            // this RelinkNode is destroyed, which is a use-after-free.
            // Joining on destruction is always safe, at worst blocking
            // destruction for as long as the burst itself takes.
            auto pairs = newly_learned_peers;
            initial_punch_thread_ = std::thread([this, pairs] {
                for (const auto& kv : pairs) {
                    nat_punch(kv.second, *kv.first);
                }
            });
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

        if (mode == DiscoveryMode::RlCore) start_rlcore_reregister_thread();

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
        socket_t sock = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (sock == kInvalidSocket) throw std::runtime_error("detect_local_ip_for_peer: socket() failed");

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
        relink::close_socket(sock);
        return local_ip;
    }

    static constexpr uint32_t kAutoFrameId = 0xFFFFFFFF;

    std::mutex state_mutex_;
    DiscoveryMode mode_ = DiscoveryMode::None;
    std::unordered_set<uint32_t> declared_topics_;
    // Split out of declared_topics_ so rlcore can be told WHICH role
    // this node plays per topic (see the periodic RLPA role-announce in
    // start_rlcore_reregister_thread()), for `rl_topic.py info`'s p2p
    // connection details (who publishes, who subscribes, by ip:port). A
    // topic can be both (e.g. a loopback/echo test) -- not mutually
    // exclusive.
    std::unordered_set<uint32_t> advertised_topics_;
    std::unordered_set<uint32_t> subscribed_topics_;
    std::unordered_map<uint32_t, std::vector<PeerAddr>> peers_;
    std::unordered_map<uint32_t, uint32_t> next_image_frame_id_;
    std::unordered_map<uint32_t, uint32_t> next_compressed_image_frame_id_;
    std::unordered_map<uint32_t, std::string> topic_names_;
    std::vector<std::shared_ptr<ImageReassembler>> image_reassemblers_;
    std::vector<std::shared_ptr<CompressedImageReassembler>> compressed_image_reassemblers_;

    std::mutex start_mutex_;
    bool started_ = false;
    int data_thread_core_override_ = kAutoPinCore;
    bool use_realtime_ = false;
    int rt_priority_ = 10;
    std::atomic<bool> stop_requested_{false};

    UdpTransport transport_;
    std::unique_ptr<MulticastDiscovery> mcast_;
    uint16_t network_id_ = 0;

    bool multiplex_ = true;
    // Keyed by a 64-bit group key, not topic_id directly: an unpaired
    // topic's key is just its topic_id (fits in the low 32 bits, tag bit
    // never set since topic_id is uint32_t); a paired topic's key is
    // kPairKeyTag | pair_id instead, so multiple topic_ids sharing one
    // pair_id resolve to the SAME map entry (see pair()/transport_for()).
    // Tagging pair keys this way, rather than reusing pair_id as a raw
    // uint32_t key, avoids an arbitrary user-chosen pair_id ever
    // colliding with an unrelated topic_id that happens to have the same
    // numeric value.
    static constexpr uint64_t kPairKeyTag = uint64_t{1} << 32;
    std::unordered_map<uint32_t, uint32_t> topic_pair_id_;  // topic_id -> pair_id, paired topics only
    std::unordered_map<uint64_t, std::unique_ptr<UdpTransport>> topic_transports_;
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

    // rlcore registration is otherwise one-shot (see ensure_started()):
    // a node only ever learns the peers that existed at ITS OWN startup
    // moment, so whichever side starts earlier can never learn about a
    // peer that registers later -- publish-before-subscribe or
    // subscribe-before-publish both silently fail depending on order.
    // This periodic re-registration thread removes that ordering
    // requirement: every rlcore_reregister_interval_, re-send the same
    // RegisterRequest used at startup and merge any newly-returned
    // peers into peers_, so a late-joining peer gets picked up without
    // either side needing a restart.
    std::vector<std::pair<UdpTransport*, std::vector<uint32_t>>> rlcore_groups_;
    uint32_t rlcore_self_ip_ = 0;
    std::chrono::duration<double> rlcore_reregister_interval_{0.3};
    std::atomic<bool> rlcore_reregister_stop_{false};
    std::thread rlcore_reregister_thread_;

    // Runs the initial NAT-punch burst for peers learned at startup, off
    // the caller's own thread -- see the call site in ensure_started().
    std::thread initial_punch_thread_;

    // One background thread per newly-discovered-peer batch found by the
    // periodic reregister loop (see start_rlcore_reregister_thread()) --
    // joined in the destructor. A plain vector, not reused/pooled: these
    // bursts are rare (one per discovery event, not per tick) and each
    // is cheap to join once finished, so simplicity wins over pooling.
    std::mutex punch_threads_mutex_;
    std::vector<std::thread> punch_threads_;

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
                ::sendto(g.first->native_handle(), reinterpret_cast<const char*>(buf), static_cast<int>(len), 0,
                         reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest));
            }
        }
    }

    // chunk_topic_dir_entries(), not one encode_topic_dir_announce()
    // call: a real large-topic-count system (e.g. ~1000 topics) easily
    // exceeds kTopicDirMaxEntries (512) in one node, and a single
    // encode_topic_dir_announce() call would just return false and get
    // silently skipped, dropping every name for that node with no
    // announce ever reaching rlcore even though registration/data
    // traffic worked fine (different wire protocols) -- rl_topic
    // list/info would then see nothing despite everything else running.
    // See chunk_topic_dir_entries()'s doc comment for why entry-count
    // chunking alone isn't enough either. Fire-and-forget, no ack --
    // called both once at ensure_started() time and every tick from
    // start_rlcore_reregister_thread(), so a single lost packet doesn't
    // permanently hide the name (see that call site's comment).
    void announce_topic_names_to_rlcore() {
        std::vector<TopicDirEntry> entries;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            entries.reserve(topic_names_.size());
            for (const auto& kv : topic_names_) entries.push_back(TopicDirEntry{kv.first, kv.second});
        }
        if (entries.empty()) return;
        struct sockaddr_in dest{};
        dest.sin_family = AF_INET;
        dest.sin_addr.s_addr = htonl(set_rlcore.resolved_ip());
        dest.sin_port = htons(set_rlcore.resolved_port());
        for (const auto& chunk : chunk_topic_dir_entries(entries)) {
            uint8_t announce_buf[kTopicDirMaxPacket];
            size_t announce_len = 0;
            if (encode_topic_dir_announce(chunk, announce_buf, sizeof(announce_buf), &announce_len)) {
                ::sendto(transport_.native_handle(), reinterpret_cast<const char*>(announce_buf),
                         static_cast<int>(announce_len), 0,
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

    void start_rlcore_reregister_thread() {
        rlcore_reregister_thread_ = std::thread([this] {
            int tick = 0;
            while (!rlcore_reregister_stop_.load()) {
                std::this_thread::sleep_for(
                    std::chrono::duration_cast<std::chrono::milliseconds>(rlcore_reregister_interval_));
                if (rlcore_reregister_stop_.load()) break;
                ++tick;
                // Snapshot the group list under the lock (rlcore_groups_
                // never changes after ensure_started(), but read it
                // consistently anyway), then re-register outside it --
                // register_with_rlcore_on_socket() blocks up to its own
                // timeout, and holding state_mutex_ across that would
                // stall any publish/subscribe call on another thread.
                // This reuses each group's own data socket (same
                // NAT-traversal requirement as the initial registration);
                // passing `g.first` (the owning UdpTransport) makes it
                // wait on that transport's register-reply handoff queue
                // instead of calling recvfrom() itself, so it no longer
                // races the data thread for the same fd (that race used
                // to make a single attempt per tick miss for several
                // seconds straight at high message rates -- see
                // UdpTransport::get_register_reply()). The multi-attempt
                // retry stays regardless, since rlcore itself can still
                // be briefly slow/unreachable independent of that fixed
                // race: backoff still doubles per attempt (100ms, 200ms,
                // 400ms, 800ms, 1.6s -- ~3.1s worst case).
                std::vector<std::pair<UdpTransport*, std::vector<uint32_t>>> groups_copy;
                uint32_t self_ip;
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    groups_copy = rlcore_groups_;
                    self_ip = rlcore_self_ip_;
                }
                // Node-wide, not per-group -- repeats every tick like the
                // per-group role announce below, so a lost topic-name
                // announce (see ensure_started()'s call site) self-heals
                // instead of leaving rl_topic list/info blind to that name
                // for the rest of this node's lifetime.
                announce_topic_names_to_rlcore();
                // At many topics (e.g. 20+), re-registering every group
                // every tick multiplies load on rlcore (a single-
                // threaded server) and this node's own busy sockets by
                // the topic count, which measurably increases loss
                // instead of reducing it. Once a topic already has at
                // least one known peer, skip it on most ticks -- only
                // do a full sweep (every 10th tick) so a late-arriving
                // SECOND peer for an already-satisfied topic still
                // eventually gets picked up. A topic with no peer yet is
                // always retried every tick, same as before.
                //
                // On non-full-sweep ticks, re-register only the topics
                // in this group that still lack a peer, not the whole
                // group -- the old all-or-nothing check (skip the group
                // ONLY if every single topic has a peer) meant that in
                // the default multiplex mode, where every declared topic
                // sits in ONE shared group, a single still-unsatisfied
                // topic out of e.g. 1000 made every tick re-send the
                // FULL 1000-topic RegisterRequest again. At real
                // large-system scale that self-inflicted storm (a ~4KB
                // request + a multi-KB ack, every 300ms, indefinitely)
                // starved rlcore's single-threaded recv loop badly enough
                // that the two ends' registrations kept losing the race
                // asymmetrically -- one side converged to ~99% delivery,
                // the other got stuck under 15% for the whole test, with
                // the deficit never recovering because the flood never
                // stopped. Registering only the pending subset keeps the
                // request small once most topics are already satisfied.
                bool full_sweep = (tick % 10 == 0);
                for (const auto& g : groups_copy) {
                    if (rlcore_reregister_stop_.load()) break;
                    // Tell rlcore who's publishing/subscribing each of
                    // this group's topics, every tick (not gated by
                    // full_sweep -- this is diagnostic-only for
                    // `rl_topic.py info`, not part of peer discovery,
                    // and rlcore's TTL for this data is only 10x this
                    // interval, so skipping most ticks would risk a
                    // still-alive topic flickering as "gone"). See
                    // topic_directory.hpp's role-directory section.
                    {
                        std::vector<RoleAnnounceEntry> role_entries;
                        {
                            std::lock_guard<std::mutex> lock(state_mutex_);
                            for (uint32_t topic : g.second) {
                                uint8_t role = 0;
                                if (advertised_topics_.count(topic)) role |= kRolePublisher;
                                if (subscribed_topics_.count(topic)) role |= kRoleSubscriber;
                                if (role) role_entries.push_back(RoleAnnounceEntry{topic, role});
                            }
                        }
                        for (const auto& chunk : chunk_role_announce_entries(role_entries)) {
                            uint8_t buf[kRoleMaxPacket];
                            size_t len = 0;
                            if (encode_role_announce(chunk, buf, sizeof(buf), &len)) {
                                sockaddr_in dst{};
                                dst.sin_family = AF_INET;
                                dst.sin_addr.s_addr = htonl(set_rlcore.resolved_ip());
                                dst.sin_port = htons(set_rlcore.resolved_port());
                                ::sendto(g.first->native_handle(), reinterpret_cast<const char*>(buf),
                                         static_cast<int>(len), 0,
                                         reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
                            }
                        }
                    }
                    std::vector<uint32_t> pending;
                    if (!full_sweep) {
                        std::lock_guard<std::mutex> lock(state_mutex_);
                        for (uint32_t topic : g.second) {
                            auto it = peers_.find(topic);
                            if (it == peers_.end() || it->second.empty()) pending.push_back(topic);
                        }
                        if (pending.empty()) continue;
                    }
                    const std::vector<uint32_t>& to_register = full_sweep ? g.second : pending;
                    auto t0 = std::chrono::steady_clock::now();
                    auto outcome = register_with_rlcore_on_socket(
                        g.first->native_handle(),
                        set_rlcore.resolved_ip(), set_rlcore.resolved_port(),
                        self_ip, g.first->local_port(),
                        to_register.data(), static_cast<uint16_t>(to_register.size()),
                        /*max_retries=*/5, /*timeout_ms=*/100, g.first,
                        set_rlcore.encrypt_key());
                    if (!outcome.ok) continue;
                    // Populate peers_ for EVERY entry in this ack first,
                    // fast and lock-only, before punching anything.
                    // nat_punch() is 3 packets * 30ms sleep = ~90ms per
                    // peer -- calling it inline, per-peer, in this same
                    // loop (the old code) meant that when a registration
                    // burst returns many newly-discovered peers at once
                    // (e.g. all ~1000 topics' worth, the common case the
                    // very first time this side learns about an already-
                    // registered remote node), populating peer #2's entry
                    // was gated behind peer #1's 90ms punch delay, peer #3
                    // behind #1+#2, and so on -- throttling the whole
                    // peers_ map to ~11 entries/second regardless of how
                    // many were actually ready immediately. Since
                    // publish() can only send once a topic's peers_ entry
                    // exists, that throttling alone was enough to make
                    // most of a real ~1000-topic node's topics never
                    // acquire a usable peer within a typical test/run
                    // window -- this, not registration failures or a
                    // discovery "race", was the actual cause of one side
                    // of a two-node exchange measuring ~10% delivery
                    // while the other measured ~99%+ for what should be a
                    // symmetric workload. Collecting the newly-discovered
                    // peers here and punching them all in one background
                    // burst afterward (same pattern as the initial-
                    // registration punch burst in ensure_started()) keeps
                    // population instant regardless of batch size.
                    std::vector<std::pair<PeerAddr, UdpTransport*>> newly_discovered;
                    // Adds `addr` to peers_[topic_id] iff not already
                    // present, returning whether it was new -- shared by
                    // both the primary address below and its optional
                    // same-NAT LAN candidate (see RegisterAckPeer's doc
                    // comment in wire.hpp), so both get exactly the same
                    // dedup-against-known-peers treatment.
                    auto add_if_new = [&](uint32_t topic_id, const PeerAddr& addr) {
                        std::lock_guard<std::mutex> lock(state_mutex_);
                        auto& known = peers_[topic_id];
                        for (const auto& existing : known) {
                            if (existing.ip_host_order == addr.ip_host_order && existing.port == addr.port) {
                                return false;
                            }
                        }
                        known.push_back(addr);
                        return true;
                    };
                    for (const auto& p : outcome.peers) {
                        // p.topic_id is a misaligned field in a
                        // #pragma pack(1) struct (RegisterAckPeer in
                        // wire.hpp) -- copy fields out before using them,
                        // not take a reference to the packed field
                        // itself (that's UB and has actually crashed
                        // with a general protection fault at scale).
                        uint32_t ip = p.ip;
                        uint16_t port = p.port;
                        uint32_t topic_id = p.topic_id;
                        uint32_t lan_ip = p.lan_ip;
                        uint16_t lan_port = p.lan_port;
                        PeerAddr addr{ip, port};
                        if (add_if_new(topic_id, addr)) newly_discovered.push_back({addr, g.first});
                        if (lan_ip != 0 && !(lan_ip == ip && lan_port == port)) {
                            PeerAddr lan_addr{lan_ip, lan_port};
                            if (add_if_new(topic_id, lan_addr)) newly_discovered.push_back({lan_addr, g.first});
                        }
                    }
                    if (!newly_discovered.empty()) {
                        std::lock_guard<std::mutex> lock(punch_threads_mutex_);
                        punch_threads_.push_back(std::thread([this, newly_discovered] {
                            for (const auto& kv : newly_discovered) {
                                nat_punch(kv.first, *kv.second);
                            }
                        }));
                    }
                }
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

// Step 6: the public ReLink API -- RelinkNode, wiring together the ring
// buffer (step 2), UDP data thread (step 3), com-core client (step 4),
// and multicast discovery (step 5) behind the templated
// advertise/subscribe/publish<T> surface shown in relink_example.cpp.

#pragma once

#include "relink/wire.hpp"
#include "relink/udp_transport.hpp"
#include "relink/com_core_client.hpp"
#include "relink/multicast_discovery.hpp"
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

enum class DiscoveryMode { None, ComCore, Multicast };

class RelinkNode {
public:
    // --- mode A config sub-object, per spec: separate ip()/port()
    // setters, port has a default, mode selection happens at the ip()
    // call itself (the setter call, not deferred to spin()/start()). ---
    class ComCoreConfig {
    public:
        explicit ComCoreConfig(RelinkNode& owner) : owner_(owner) {}

        void ip(const std::string& addr) {
            owner_.select_mode(DiscoveryMode::ComCore);
            com_core_ip_ = ipv4_to_host_order(addr);
            ip_set_ = true;
        }

        void port(uint16_t p = kComCoreDefaultPort) {
            com_core_port_ = p;
        }

        bool ip_is_set() const { return ip_set_; }
        uint32_t resolved_ip() const { return com_core_ip_; }
        uint16_t resolved_port() const { return com_core_port_; }

    private:
        RelinkNode& owner_;
        bool ip_set_ = false;
        uint32_t com_core_ip_ = 0;
        uint16_t com_core_port_ = kComCoreDefaultPort;
    };

    RelinkNode() : set_com_core(*this) {}

    ComCoreConfig set_com_core;

    // --- mode B, mutually exclusive with mode A (per spec) ---
    void use_multicast_discovery() {
        select_mode(DiscoveryMode::Multicast);
    }

    // --- advertise: publisher-side topic declaration ---
    template <typename T>
    void advertise(uint16_t topic_id, bool /*secure*/ = false, bool /*checksum*/ = false) {
        static_assert(std::is_trivially_copyable<T>::value,
                      "advertise<T>: T must be trivially copyable");
        std::lock_guard<std::mutex> lock(state_mutex_);
        declared_topics_.insert(topic_id);
    }

    // --- subscribe: receiver-side topic declaration + typed callback,
    // invoked inline on the data thread per spec's v1 threading design ---
    template <typename T, typename Callback>
    void subscribe(uint16_t topic_id, Callback callback, bool /*secure*/ = false) {
        static_assert(std::is_trivially_copyable<T>::value,
                      "subscribe<T>: T must be trivially copyable");
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
    bool publish(uint16_t topic_id, const T& value) {
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
    std::vector<PeerAddr> peers_for_topic(uint16_t topic_id) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = peers_.find(topic_id);
        return it != peers_.end() ? it->second : std::vector<PeerAddr>{};
    }

private:
    friend class ComCoreConfig;

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
                "no discovery method configured -- call set_com_core.ip(...) or "
                "use_multicast_discovery() before spin()/publish()/subscribe traffic");
        }
        if (mode == DiscoveryMode::ComCore && !set_com_core.ip_is_set()) {
            throw std::runtime_error("com-core IP not set -- call set_com_core.ip(...)");
        }

        transport_.bind(0);
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

        std::vector<uint16_t> topics;
        {
            std::lock_guard<std::mutex> lock2(state_mutex_);
            topics.assign(declared_topics_.begin(), declared_topics_.end());
        }

        if (mode == DiscoveryMode::ComCore) {
            uint32_t self_ip = detect_local_ip_for_peer(set_com_core.resolved_ip(),
                                                         set_com_core.resolved_port());
            auto outcome = register_with_com_core(
                set_com_core.resolved_ip(), set_com_core.resolved_port(),
                self_ip, transport_.local_port(),
                topics.data(), static_cast<uint16_t>(topics.size()));
            if (outcome.ok) {
                std::lock_guard<std::mutex> lock2(state_mutex_);
                for (const auto& p : outcome.peers) {
                    peers_[p.topic_id].push_back(PeerAddr{p.ip, p.port});
                }
            }
            // If registration failed after retries, register_with_com_core
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
            mcast_->set_peer_discovered_callback([this](uint16_t topic, const PeerInfo& p) {
                std::lock_guard<std::mutex> lock2(state_mutex_);
                peers_[topic].push_back(PeerAddr{p.ip, p.port});
            });
            mcast_->start();
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

    std::mutex state_mutex_;
    DiscoveryMode mode_ = DiscoveryMode::None;
    std::unordered_set<uint16_t> declared_topics_;
    std::unordered_map<uint16_t, std::vector<PeerAddr>> peers_;

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

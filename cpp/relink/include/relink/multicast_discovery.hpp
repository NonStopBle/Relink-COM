// Step 5 (part 2): one-shot jittered multicast beacon discovery, per
// relink-com-spec.md "Mode B" section.
//
// - Sender: on start(), sends its beacon 3x with random jitter
//   (0-200ms between attempts) to avoid boot-storm collisions, then
//   re-sends sparsely every [reannounce_min_ms, reannounce_max_ms]
//   (spec default 30-60s; test code overrides this to keep tests fast).
// - Listener: parses incoming BeaconPackets on its own thread, matches
//   topic_ids against the locally-declared publish/subscribe topic set;
//   overlap -> store/update topic_id -> ip:port in the peer table;
//   no overlap -> discard immediately, no state kept.
// - This class intentionally does not touch the per-message data path
//   (spec: discovery must never share a thread with the data path) --
//   it runs its own sender + listener threads only.

#pragma once

#include "relink/beacon.hpp"
#include "relink/topic_directory.hpp"
#include <atomic>
#include <cstdio>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <random>
#include <chrono>
#include <functional>
#include <cstring>
#include <stdexcept>
#include <string>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

namespace relink {

inline constexpr char kDefaultMulticastGroup[] = "239.255.0.1";
inline constexpr uint16_t kDefaultMulticastPort = 7400;

// Derives a distinct multicast GROUP ADDRESS per network_id, so nodes
// configured with different network_ids never even receive each
// other's beacons at the OS/kernel level -- true isolation, unlike a
// payload field a node would have to decode-then-discard. This is
// RelinkNode's ROS_DOMAIN_ID-style equivalent (see
// RelinkNode::set_network_id()). network_id=0 (the default) maps to
// exactly today's fixed kDefaultMulticastGroup, so existing
// single-domain deployments that never call set_network_id() see zero
// behavior change -- every other network_id maps to a distinct
// 239.255.x.y address, offset by +1 so no non-zero network_id can ever
// collide with the reserved network_id=0 address (239.255.0.1).
// No wire-format change: the beacon packet itself is untouched.
//
// IMPORTANT: address alone is NOT sufficient isolation and must always
// be paired with derive_multicast_port() below -- see that function's
// comment for why (a real, reproduced Linux SO_REUSEPORT + multicast
// kernel behavior, not a theoretical concern).
inline std::string derive_multicast_group(uint16_t network_id) {
    if (network_id == 0) return kDefaultMulticastGroup;
    uint32_t computed = static_cast<uint32_t>(network_id) + 1;
    unsigned hi = (computed >> 8) & 0xFF;
    unsigned lo = computed & 0xFF;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "239.255.%u.%u", hi, lo);
    return std::string(buf);
}

// Derives a distinct multicast PORT per network_id -- REQUIRED, not
// just cosmetic: MulticastDiscovery's listener socket sets
// SO_REUSEPORT (so several ReLink nodes can share one host on the same
// multicast port), and on Linux, SO_REUSEPORT's hash-based delivery
// selection is scoped by LOCAL PORT ONLY -- two sockets bound to the
// same port but joined to DIFFERENT multicast group addresses were
// directly observed (raw socket test, this machine, kernel default
// config) to both receive a packet sent to only one of those groups.
// Varying only the group address (derive_multicast_group() above)
// does NOT isolate two network_ids that share a host; varying the port
// too sidesteps the SO_REUSEPORT interaction entirely, since different
// ports never join the same reuseport group in the first place.
//
// network_id=0 maps to kDefaultMulticastPort unchanged. Other values
// map injectively for network_id in [1, 65535 - kDefaultMulticastPort]
// (~58135 distinct domains); beyond that the mapping wraps (a network_id
// and some other, much larger network_id could then share a port) --
// an accepted, documented limit, same spirit as ROS_DOMAIN_ID's own
// practical range limit from its port-arithmetic formula.
inline uint16_t derive_multicast_port(uint16_t network_id) {
    if (network_id == 0) return kDefaultMulticastPort;
    constexpr uint32_t kRange = 65535 - kDefaultMulticastPort;  // ports stay <= 65535
    uint32_t offset = (static_cast<uint32_t>(network_id) % kRange) + 1;  // 1..kRange, never 0
    return static_cast<uint16_t>(kDefaultMulticastPort + offset);
}

struct PeerInfo {
    uint32_t ip;   // host byte order
    uint16_t port;
};

// Callback invoked whenever a topic's peer is discovered or updated.
using PeerDiscoveredCallback = std::function<void(uint32_t topic_id, const PeerInfo& peer)>;

struct MulticastDiscoveryConfig {
    std::string group_ip = kDefaultMulticastGroup;
    uint16_t group_port = kDefaultMulticastPort;

    uint32_t self_ip = 0;      // host byte order -- this node's own IP

    // One beacon is sent per group, each with its own port. Multiplexed
    // nodes (the default) have exactly one group covering every declared
    // topic on the node's single shared data port. A node running with
    // set_multiplex(false) (see relink.hpp) instead has one group per
    // topic, each carrying that topic's own dedicated port -- letting
    // peers learn a per-topic port the same way rostopic-style
    // one-port-per-topic nodes do, without changing the beacon's wire
    // format at all (still just node_ip + node_port + topic_ids).
    struct PortGroup {
        uint16_t port = 0;
        std::vector<uint32_t> topics;
    };
    std::vector<PortGroup> port_groups;

    // Spec defaults: 3x jittered startup burst (0-200ms between sends),
    // then sparse 30-60s re-announce. Tests override these to be fast.
    int startup_burst_count = 3;
    int startup_jitter_max_ms = 200;
    int reannounce_min_ms = 30000;
    int reannounce_max_ms = 60000;
};

class MulticastDiscovery {
public:
    explicit MulticastDiscovery(MulticastDiscoveryConfig cfg) : cfg_(std::move(cfg)) {
        for (const auto& g : cfg_.port_groups) {
            self_ports_.insert(g.port);
            for (uint32_t t : g.topics) local_topics_.insert(t);
        }
    }
    ~MulticastDiscovery() { stop(); }

    MulticastDiscovery(const MulticastDiscovery&) = delete;
    MulticastDiscovery& operator=(const MulticastDiscovery&) = delete;

    void set_peer_discovered_callback(PeerDiscoveredCallback cb) {
        on_peer_discovered_ = std::move(cb);
    }

    // Lets RelinkNode answer rl_topic's "what topic names do you know?"
    // queries without MulticastDiscovery needing to know anything about
    // RelinkNode's internal registry -- called from the listener thread
    // whenever an RLNQ query arrives, must be safe to call from there.
    void set_topic_name_provider(std::function<std::vector<TopicDirEntry>()> provider) {
        name_provider_ = std::move(provider);
    }

    void start() {
        if (running_.exchange(true)) return;
        setup_send_socket();
        setup_recv_socket();
        sender_thread_ = std::thread([this] { sender_loop(); });
        listener_thread_ = std::thread([this] { listener_loop(); });
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (sender_thread_.joinable()) sender_thread_.join();
        if (listener_thread_.joinable()) listener_thread_.join();
        if (send_sock_ >= 0) { ::close(send_sock_); send_sock_ = -1; }
        if (recv_sock_ >= 0) { ::close(recv_sock_); recv_sock_ = -1; }
    }

    // Snapshot of the current peer table for one topic (empty if none).
    std::vector<PeerInfo> peers_for_topic(uint32_t topic_id) {
        std::lock_guard<std::mutex> lock(table_mutex_);
        std::vector<PeerInfo> result;
        auto it = table_.find(topic_id);
        if (it != table_.end()) {
            for (const auto& kv : it->second) {
                result.push_back(PeerInfo{kv.first.first, kv.first.second});
            }
        }
        return result;
    }

    size_t known_topic_count() {
        std::lock_guard<std::mutex> lock(table_mutex_);
        return table_.size();
    }

    // Every topic_id seen in ANY beacon from ANY peer so far, regardless
    // of whether this node itself declared it -- this is what lets
    // rltopic_list() show topics other nodes have, not just our own
    // (see relink.hpp). Only the numeric id crosses the wire (beacons
    // never carry names), so a topic learned this way has no name here;
    // RelinkNode::rltopic_list() fills one in only if this same process
    // separately resolved that id itself via topic_id_for().
    std::vector<uint32_t> all_known_topic_ids() {
        std::lock_guard<std::mutex> lock(network_topics_mutex_);
        return std::vector<uint32_t>(network_topics_.begin(), network_topics_.end());
    }

private:
    void setup_send_socket() {
        send_sock_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (send_sock_ < 0) throw std::runtime_error("MulticastDiscovery: send socket() failed");

        // Loop back to this host too -- required so localhost-only tests
        // (both "nodes" as threads in the same process/machine) can see
        // their own multicast traffic; real deployments across machines
        // don't depend on this flag.
        unsigned char loop = 1;
        ::setsockopt(send_sock_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

        int ttl = 1; // stay on the local subnet, per typical LAN discovery use
        ::setsockopt(send_sock_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    }

    void setup_recv_socket() {
        recv_sock_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (recv_sock_ < 0) throw std::runtime_error("MulticastDiscovery: recv socket() failed");

        int reuse = 1;
        ::setsockopt(recv_sock_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
        ::setsockopt(recv_sock_, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif

        struct sockaddr_in bind_addr{};
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_addr.s_addr = INADDR_ANY;
        bind_addr.sin_port = htons(cfg_.group_port);
        if (::bind(recv_sock_, reinterpret_cast<struct sockaddr*>(&bind_addr), sizeof(bind_addr)) < 0) {
            throw std::runtime_error("MulticastDiscovery: bind() failed");
        }

        struct ip_mreq mreq{};
        ::inet_pton(AF_INET, cfg_.group_ip.c_str(), &mreq.imr_multiaddr);
        mreq.imr_interface.s_addr = INADDR_ANY;
        if (::setsockopt(recv_sock_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
            throw std::runtime_error("MulticastDiscovery: IP_ADD_MEMBERSHIP failed");
        }

        // Bounded recv timeout, same rationale as UdpTransport: lets the
        // listener loop check the stop flag promptly instead of blocking
        // forever.
        struct timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 50 * 1000;
        ::setsockopt(recv_sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    void send_beacon_once() {
        struct sockaddr_in dest{};
        dest.sin_family = AF_INET;
        ::inet_pton(AF_INET, cfg_.group_ip.c_str(), &dest.sin_addr);
        dest.sin_port = htons(cfg_.group_port);

        // One beacon per port group -- see PortGroup's comment above.
        for (const auto& g : cfg_.port_groups) {
            uint8_t buf[512];
            size_t len = 0;
            auto er = encode_beacon_packet(cfg_.self_ip, g.port,
                                            g.topics.data(),
                                            static_cast<uint16_t>(g.topics.size()),
                                            buf, sizeof(buf), &len);
            if (er != BeaconEncodeResult::Ok) continue;
            ::sendto(send_sock_, buf, len, 0, reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest));
        }
    }

    void sender_loop() {
        std::mt19937 rng(std::random_device{}());

        // Startup burst: N sends with random jitter between them.
        for (int i = 0; i < cfg_.startup_burst_count && running_.load(); ++i) {
            send_beacon_once();
            if (i + 1 < cfg_.startup_burst_count) {
                std::uniform_int_distribution<int> jitter(0, cfg_.startup_jitter_max_ms);
                sleep_ms_interruptible(jitter(rng));
            }
        }

        // Sparse re-announce: sleep a random interval in [min, max], then
        // beacon again, repeat until stopped.
        std::uniform_int_distribution<int> reannounce(cfg_.reannounce_min_ms, cfg_.reannounce_max_ms);
        while (running_.load()) {
            int wait_ms = reannounce(rng);
            sleep_ms_interruptible(wait_ms);
            if (!running_.load()) break;
            send_beacon_once();
        }
    }

    // Sleeps up to `total_ms`, but wakes early (in small increments) if
    // running_ is cleared -- keeps stop() responsive even mid-backoff.
    void sleep_ms_interruptible(int total_ms) {
        const int step_ms = 20;
        int slept = 0;
        while (slept < total_ms && running_.load()) {
            int chunk = std::min(step_ms, total_ms - slept);
            std::this_thread::sleep_for(std::chrono::milliseconds(chunk));
            slept += chunk;
        }
    }

    void listener_loop() {
        uint8_t buf[512];
        while (running_.load()) {
            struct sockaddr_in src{};
            socklen_t src_len = sizeof(src);
            ssize_t n = ::recvfrom(recv_sock_, buf, sizeof(buf), 0,
                                    reinterpret_cast<struct sockaddr*>(&src), &src_len);
            if (n <= 0) continue;

            // rl_topic's name-directory protocol shares this port but is
            // tagged with its own magic (see topic_directory.hpp) so it
            // can never be mistaken for a BeaconPacket -- check that
            // first, and only fall through to beacon decoding otherwise.
            TopicDirKind dir_kind = topic_dir_packet_kind(buf, static_cast<size_t>(n));
            if (dir_kind == TopicDirKind::Query) {
                if (name_provider_) {
                    std::vector<TopicDirEntry> entries = name_provider_();
                    uint8_t reply_buf[kTopicDirMaxPacket];
                    size_t reply_len = 0;
                    if (encode_topic_dir_reply(entries, reply_buf, sizeof(reply_buf), &reply_len)) {
                        ::sendto(send_sock_, reply_buf, reply_len, 0,
                                 reinterpret_cast<struct sockaddr*>(&src), src_len);
                    }
                }
                continue;
            }
            if (dir_kind == TopicDirKind::Announce || dir_kind == TopicDirKind::Reply) {
                continue; // not collected locally -- only rl_topic consumes these
            }

            DecodedBeacon b{};
            if (decode_beacon_packet(buf, static_cast<size_t>(n), &b) != BeaconDecodeResult::Ok) {
                continue; // malformed / non-ReLink traffic on this port: drop
            }

            // Ignore our own beacon (loopback delivers it to ourselves too) --
            // any of our own port groups' ports counts as "ours" now that a
            // demultiplexed node beacons from several ports, not just one.
            if (b.node_ip == cfg_.self_ip && self_ports_.count(b.node_port)) continue;

            for (uint16_t i = 0; i < b.topic_count; ++i) {
                uint32_t topic = beacon_topic_at(b, i);

                // Record every topic id we ever see, regardless of local
                // interest -- this is a network-wide "what topics exist"
                // view (rltopic_list()), separate from the peer-routing
                // table below (which only tracks topics WE need peers
                // for, per the original discovery-overlap design).
                {
                    std::lock_guard<std::mutex> lock(network_topics_mutex_);
                    network_topics_.insert(topic);
                }

                if (!local_topics_.count(topic)) continue; // no overlap: discard, keep no peer-routing state

                PeerInfo peer{b.node_ip, b.node_port};
                bool is_new;
                {
                    std::lock_guard<std::mutex> lock(table_mutex_);
                    auto& peer_set = table_[topic];
                    auto key = std::make_pair(peer.ip, peer.port);
                    is_new = peer_set.insert({key, true}).second;
                }
                if (is_new && on_peer_discovered_) {
                    on_peer_discovered_(topic, peer);
                }
            }
        }
    }

    MulticastDiscoveryConfig cfg_;
    std::unordered_set<uint32_t> local_topics_;
    std::unordered_set<uint16_t> self_ports_;

    std::atomic<bool> running_{false};
    int send_sock_ = -1;
    int recv_sock_ = -1;
    std::thread sender_thread_;
    std::thread listener_thread_;

    struct PairHash {
        size_t operator()(const std::pair<uint32_t, uint16_t>& p) const {
            return std::hash<uint64_t>{}((uint64_t(p.first) << 16) | p.second);
        }
    };
    std::mutex table_mutex_;
    std::unordered_map<uint32_t, std::unordered_map<std::pair<uint32_t, uint16_t>, bool, PairHash>> table_;

    std::mutex network_topics_mutex_;
    std::unordered_set<uint32_t> network_topics_;

    PeerDiscoveredCallback on_peer_discovered_;
    std::function<std::vector<TopicDirEntry>()> name_provider_;
};

} // namespace relink

// rl_topic -- a rostopic-style CLI for ReLink's topic name directory,
// C++ build. Mirrors rl_topic.py's subcommands and flags exactly (same
// wire protocol, same cache file, same output shape) -- see rl_topic.py
// for the full rationale on what maps from rostopic and what doesn't
// (ReLink has no message-type registry, so `type`/`find` aren't
// meaningful here; echo/hz/bw work on raw bytes instead).
//
// Build:
//   g++ -std=c++17 -I relink/include -pthread rl_topic.cpp -o rl_topic
//
// Usage: rl_topic list | info <topic> | hz <topic> | bw <topic> |
//        echo <topic> | pub <topic> --hex <bytes> | --text <string>
//   [--rlcore-ip <ip>] [--rlcore-port <port>] [--group <ip>] [--port <n>]
//   [--timeout <s>] [--no-cache] [--refresh]

#include "relink/relink.hpp"
#include "relink/topic_directory.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <fstream>
#include <sstream>
#include <cmath>
#include <algorithm>
#include <csignal>

#include "relink/platform.hpp"
#include <sys/stat.h>
#ifndef _WIN32
#include <pwd.h>
#include <unistd.h>
#endif

using namespace relink;

// --------------------------------------------------------------------
// Args
// --------------------------------------------------------------------
struct Args {
    std::string command;
    std::string topic;
    std::string rlcore_ip;
    uint16_t rlcore_port = 8445;
    std::string group = kDefaultMulticastGroup;
    uint16_t port = kDefaultMulticastPort;
    double timeout = 1.5;
    bool no_cache = false;
    bool refresh = false;
    int window = 100;
    double report_every = 5.0;
    int count = 0;
    std::string hex_payload;
    std::string text_payload;
    bool have_hex = false, have_text = false;
    int repeat = 1;
    double rate_period = 1.0;
};

static bool next_arg(int argc, char** argv, int& i, std::string* out) {
    if (i + 1 >= argc) return false;
    *out = argv[++i];
    return true;
}

static Args parse_args(int argc, char** argv) {
    Args a;
    if (argc < 2) {
        std::fprintf(stderr, "usage: rl_topic <list|info|hz|bw|echo|pub> [topic] [options]\n");
        std::exit(2);
    }
    a.command = argv[1];
    std::vector<std::string> positional;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        std::string val;
        if (arg == "--rlcore-ip") { next_arg(argc, argv, i, &val); a.rlcore_ip = val; }
        else if (arg == "--rlcore-port") { next_arg(argc, argv, i, &val); a.rlcore_port = static_cast<uint16_t>(std::atoi(val.c_str())); }
        else if (arg == "--group") { next_arg(argc, argv, i, &val); a.group = val; }
        else if (arg == "--port") { next_arg(argc, argv, i, &val); a.port = static_cast<uint16_t>(std::atoi(val.c_str())); }
        else if (arg == "--timeout") { next_arg(argc, argv, i, &val); a.timeout = std::atof(val.c_str()); }
        else if (arg == "--no-cache") { a.no_cache = true; }
        else if (arg == "--refresh") { a.refresh = true; }
        else if (arg == "--window") { next_arg(argc, argv, i, &val); a.window = std::atoi(val.c_str()); }
        else if (arg == "--report-every") { next_arg(argc, argv, i, &val); a.report_every = std::atof(val.c_str()); }
        else if (arg == "-n" || arg == "--count") { next_arg(argc, argv, i, &val); a.count = std::atoi(val.c_str()); }
        else if (arg == "--hex") { next_arg(argc, argv, i, &val); a.hex_payload = val; a.have_hex = true; }
        else if (arg == "--text") { next_arg(argc, argv, i, &val); a.text_payload = val; a.have_text = true; }
        else if (arg == "-r" || arg == "--repeat") { next_arg(argc, argv, i, &val); a.repeat = std::atoi(val.c_str()); }
        else if (arg == "--rate-period") { next_arg(argc, argv, i, &val); a.rate_period = std::atof(val.c_str()); }
        else if (!arg.empty() && arg[0] != '-') { positional.push_back(arg); }
        else { std::fprintf(stderr, "rl_topic: unknown option %s\n", arg.c_str()); std::exit(2); }
    }
    if (!positional.empty()) a.topic = positional[0];
    return a;
}

// --------------------------------------------------------------------
// Cache (~/.cache/relink/topic_names.json -- shared file/format with
// rl_topic.py, deliberately simple enough that either tool can read/
// write it without a JSON library: one "id": "name" pair per line).
// --------------------------------------------------------------------
static std::string cache_path() {
#ifdef _WIN32
    // Forward slashes work fine in Windows file APIs (CreateFile,
    // fopen, mkdir) and let the '/'-splitting mkdir-p logic below stay
    // identical on both platforms.
    const char* home = std::getenv("LOCALAPPDATA");
    if (!home) home = std::getenv("USERPROFILE");
    if (!home) home = "C:/Temp";
    std::string h(home);
    for (char& c : h) if (c == '\\') c = '/';
    return h + "/relink/topic_names.json";
#else
    const char* home = std::getenv("HOME");
    if (!home) {
        struct passwd* pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : "/tmp";
    }
    return std::string(home) + "/.cache/relink/topic_names.json";
#endif
}

static std::string json_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

static std::map<uint32_t, std::string> load_cache() {
    std::map<uint32_t, std::string> out;
    std::ifstream f(cache_path());
    if (!f) return out;
    std::string line;
    while (std::getline(f, line)) {
        auto colon = line.find(':');
        auto q1 = line.find('"');
        if (q1 == std::string::npos || colon == std::string::npos) continue;
        auto q2 = line.find('"', q1 + 1);
        if (q2 == std::string::npos || q2 >= colon) continue;
        std::string id_str = line.substr(q1 + 1, q2 - q1 - 1);
        auto q3 = line.find('"', colon + 1);
        if (q3 == std::string::npos) continue;
        auto q4 = line.rfind('"');
        if (q4 <= q3) continue;
        std::string name = line.substr(q3 + 1, q4 - q3 - 1);
        try {
            out[static_cast<uint32_t>(std::stoul(id_str))] = name;
        } catch (...) {}
    }
    return out;
}

static void save_cache(const std::map<uint32_t, std::string>& cache) {
    std::string path = cache_path();
    auto slash = path.find_last_of('/');
    if (slash != std::string::npos) {
        std::string dir = path.substr(0, slash);
        // mkdir -p, minimal: create each path component.
        std::string accum;
        std::stringstream ss(dir);
        std::string part;
        while (std::getline(ss, part, '/')) {
            if (part.empty()) { accum += "/"; continue; }
            accum += part + "/";
#ifdef _WIN32
            ::mkdir(accum.c_str());
#else
            ::mkdir(accum.c_str(), 0755);
#endif
        }
    }
    std::ofstream f(path, std::ios::trunc);
    f << "{\n";
    size_t i = 0;
    for (const auto& kv : cache) {
        f << "  \"" << kv.first << "\": \"" << json_escape(kv.second) << "\"";
        if (++i != cache.size()) f << ",";
        f << "\n";
    }
    f << "}\n";
}

// --------------------------------------------------------------------
// Directory queries
// --------------------------------------------------------------------
static std::map<uint32_t, std::string> query_rlcore(const std::string& ip, uint16_t port, double timeout_s) {
    std::map<uint32_t, std::string> out;
    socket_t s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s == kInvalidSocket) return out;
    set_recv_timeout_ms(s, static_cast<long>(timeout_s * 1000));

    struct sockaddr_in dest{};
    dest.sin_family = AF_INET;
    ::inet_pton(AF_INET, ip.c_str(), &dest.sin_addr);
    dest.sin_port = htons(port);

    uint8_t query_buf[8];
    size_t query_len = 0;
    encode_topic_dir_query(query_buf, sizeof(query_buf), &query_len);
    ::sendto(s, reinterpret_cast<const char*>(query_buf), static_cast<int>(query_len), 0,
             reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest));

    // rlcore chunks its reply into multiple RLNR packets when it knows
    // more than kTopicDirMaxEntries (512) names (a real large-topic-
    // count fleet easily exceeds that), so collect every reply that
    // arrives, not just the first packet -- a short quiet-window after
    // the most recent chunk, not the full timeout, so a multi-chunk
    // reply doesn't force waiting out the entire timeout_s after the
    // last real chunk already arrived.
    bool got_any = false;
    while (true) {
        uint8_t buf[kTopicDirMaxPacket];
        ssize_t n = static_cast<ssize_t>(::recvfrom(s, reinterpret_cast<char*>(buf),
                                          static_cast<int>(sizeof(buf)), 0, nullptr, nullptr));
        if (n <= 0) break; // timed out -- done collecting chunks
        if (topic_dir_packet_kind(buf, static_cast<size_t>(n)) != TopicDirKind::Reply) continue;
        std::vector<TopicDirEntry> entries;
        if (decode_topic_dir_entries(buf, static_cast<size_t>(n), &entries)) {
            got_any = true;
            for (const auto& e : entries) out[e.topic_id] = e.name;
        }
        double quiet_s = timeout_s < 0.3 ? timeout_s : 0.3;
        set_recv_timeout_ms(s, static_cast<long>(quiet_s * 1000));
    }
    if (!got_any) {
        std::fprintf(stderr, "rl_topic: no reply from rlcore at %s:%u\n", ip.c_str(), port);
    }
    relink::close_socket(s);
    return out;
}

static std::map<uint32_t, std::string> query_multicast(const std::string& group, uint16_t port, double timeout_s) {
    std::map<uint32_t, std::string> out;
    socket_t s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s == kInvalidSocket) return out;
    int reuse = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    set_recv_timeout_ms(s, static_cast<long>(timeout_s * 1000));

    struct sockaddr_in dest{};
    dest.sin_family = AF_INET;
    ::inet_pton(AF_INET, group.c_str(), &dest.sin_addr);
    dest.sin_port = htons(port);

    uint8_t query_buf[8];
    size_t query_len = 0;
    encode_topic_dir_query(query_buf, sizeof(query_buf), &query_len);
    ::sendto(s, reinterpret_cast<const char*>(query_buf), static_cast<int>(query_len), 0,
             reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout_s);
    while (std::chrono::steady_clock::now() < deadline) {
        uint8_t buf[kTopicDirMaxPacket];
        ssize_t n = static_cast<ssize_t>(::recvfrom(s, reinterpret_cast<char*>(buf),
                                          static_cast<int>(sizeof(buf)), 0, nullptr, nullptr));
        if (n <= 0) break; // timed out
        if (topic_dir_packet_kind(buf, static_cast<size_t>(n)) != TopicDirKind::Reply) continue;
        std::vector<TopicDirEntry> entries;
        if (decode_topic_dir_entries(buf, static_cast<size_t>(n), &entries)) {
            for (const auto& e : entries) out[e.topic_id] = e.name;
        }
    }
    relink::close_socket(s);
    return out;
}

struct RolePeer {
    std::string ip;
    uint16_t port;
    uint8_t role;
};

// Unicast RLPQ to rlcore, return topic_id -> peers with roles -- who's
// publishing/subscribing each topic (see topic_directory.hpp's role
// directory). rlcore-only: multicast mode has no central table to ask,
// each node only knows about itself.
static std::map<uint32_t, std::vector<RolePeer>> query_rlcore_roles(
    const std::string& ip, uint16_t port, double timeout_s) {
    std::map<uint32_t, std::vector<RolePeer>> out;
    socket_t s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s == kInvalidSocket) return out;
    set_recv_timeout_ms(s, static_cast<long>(timeout_s * 1000));

    struct sockaddr_in dest{};
    dest.sin_family = AF_INET;
    ::inet_pton(AF_INET, ip.c_str(), &dest.sin_addr);
    dest.sin_port = htons(port);

    uint8_t query_buf[8];
    size_t query_len = 0;
    encode_role_query(query_buf, sizeof(query_buf), &query_len);
    ::sendto(s, reinterpret_cast<const char*>(query_buf), static_cast<int>(query_len), 0,
             reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest));

    while (true) {
        uint8_t buf[kRoleMaxPacket];
        ssize_t n = static_cast<ssize_t>(::recvfrom(s, reinterpret_cast<char*>(buf),
                                          static_cast<int>(sizeof(buf)), 0, nullptr, nullptr));
        if (n <= 0) break;
        if (role_packet_kind(buf, static_cast<size_t>(n)) != RoleDirKind::Reply) continue;
        std::vector<RolePeerEntry> entries;
        if (decode_role_reply(buf, static_cast<size_t>(n), &entries)) {
            for (const auto& e : entries) {
                struct in_addr ia{};
                ia.s_addr = htonl(e.ip);
                out[e.topic_id].push_back(RolePeer{inet_ntoa(ia), e.port, e.role});
            }
        }
        double quiet_s = timeout_s < 0.3 ? timeout_s : 0.3;
        set_recv_timeout_ms(s, static_cast<long>(quiet_s * 1000));
    }
    relink::close_socket(s);
    return out;
}

static std::map<uint32_t, std::string> collect_names(const Args& a) {
    std::map<uint32_t, std::string> fresh = a.rlcore_ip.empty()
        ? query_multicast(a.group, a.port, a.timeout)
        : query_rlcore(a.rlcore_ip, a.rlcore_port, a.timeout);

    if (a.no_cache) return fresh;
    std::map<uint32_t, std::string> cache = a.refresh ? std::map<uint32_t, std::string>{} : load_cache();
    for (const auto& kv : fresh) cache[kv.first] = kv.second; // fresh always wins
    save_cache(cache);
    return cache;
}

// --------------------------------------------------------------------
// Subcommands
// --------------------------------------------------------------------
static void cmd_list(const Args& a) {
    auto names = collect_names(a);
    if (names.empty()) {
        std::printf("rl_topic: no topics found (%s, %.1fs timeout)\n",
                    a.rlcore_ip.empty() ? "multicast" : ("rlcore " + a.rlcore_ip).c_str(), a.timeout);
        return;
    }
    for (const auto& kv : names) {
        std::printf("%-10u  %s\n", kv.first, kv.second.empty() ? "(unnamed)" : kv.second.c_str());
    }
}

static void cmd_info(const Args& a) {
    auto names = collect_names(a);
    bool is_numeric = !a.topic.empty() &&
        (std::isdigit(static_cast<unsigned char>(a.topic[0])) || a.topic[0] == '-');

    std::vector<std::pair<uint32_t, std::string>> matches;
    if (is_numeric) {
        // A numeric topic id is always directly accessible (no name
        // lookup needed to use it), even if no node ever announced a
        // string name for it -- only string queries actually depend on
        // the directory.
        uint32_t tid = static_cast<uint32_t>(std::stoul(a.topic));
        auto it = names.find(tid);
        matches.push_back({tid, it != names.end() ? it->second : std::string()});
    } else {
        for (const auto& kv : names) if (kv.second == a.topic) matches.push_back(kv);
    }

    if (matches.empty()) {
        std::fprintf(stderr, "rl_topic: no known topic matches \"%s\"\n", a.topic.c_str());
        std::exit(1);
    }

    // p2p connection details (who's publishing/subscribing, by ip:port)
    // only exist centrally at rlcore -- multicast mode has no central
    // table to ask, each node only knows about itself.
    std::map<uint32_t, std::vector<RolePeer>> roles;
    if (!a.rlcore_ip.empty()) roles = query_rlcore_roles(a.rlcore_ip, a.rlcore_port, a.timeout);

    for (const auto& kv : matches) {
        std::printf("Topic id : %u\n", kv.first);
        std::printf("Name     : %s\n", kv.second.empty() ? "(unnamed -- numeric topic id only)" : kv.second.c_str());
        std::printf("Source   : %s\n", a.rlcore_ip.empty() ? "multicast broadcast" : ("rlcore " + a.rlcore_ip).c_str());

        if (a.rlcore_ip.empty()) continue;
        std::vector<RolePeer> peers = roles.count(kv.first) ? roles[kv.first] : std::vector<RolePeer>{};
        std::vector<RolePeer> publishers, subscribers;
        for (const auto& p : peers) {
            if (p.role & kRolePublisher) publishers.push_back(p);
            if (p.role & kRoleSubscriber) subscribers.push_back(p);
        }
        std::printf("Publishers  (%zu):\n", publishers.size());
        for (const auto& p : publishers) std::printf("  %s:%u\n", p.ip.c_str(), p.port);
        std::printf("Subscribers (%zu):\n", subscribers.size());
        for (const auto& p : subscribers) std::printf("  %s:%u\n", p.ip.c_str(), p.port);
        if (peers.empty()) {
            std::printf("(no live publisher/subscriber seen for this topic at rlcore -- "
                        "either nothing is using it right now, or it's role-less raw "
                        "registration-only traffic from before this role directory existed)\n");
        }
    }
}

static void configure_node(RelinkNode& node, const Args& a) {
    if (!a.rlcore_ip.empty()) {
        node.set_rlcore.ip(a.rlcore_ip);
        node.set_rlcore.port(a.rlcore_port);
    } else {
        node.use_multicast_discovery();
    }
}

static uint32_t resolve_topic(RelinkNode& node, const std::string& s) {
    bool numeric = !s.empty() && (std::isdigit(static_cast<unsigned char>(s[0])) || s[0] == '-');
    if (numeric) return static_cast<uint32_t>(std::stoul(s));
    return node.topic_id_for(s);
}

static std::atomic<bool> g_stop{false};
static void on_sigint(int) { g_stop.store(true); }

static void cmd_hz(const Args& a) {
    RelinkNode node; configure_node(node, a);
    uint32_t topic_id = resolve_topic(node, a.topic);
    std::mutex m;
    std::vector<double> arrivals;
    node.subscribe_raw(topic_id, [&](const uint8_t*, size_t) {
        std::lock_guard<std::mutex> lk(m);
        arrivals.push_back(std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    });
    node.spin_once();
    std::fprintf(stderr, "subscribed to topic id %u (%s), waiting for messages (Ctrl-C to stop)...\n",
                 topic_id, a.topic.c_str());
    std::signal(SIGINT, on_sigint);

    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        node.spin_once();
        std::vector<double> recent;
        {
            std::lock_guard<std::mutex> lk(m);
            double cutoff = std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch()).count() - a.report_every;
            for (double t : arrivals) if (t >= cutoff) recent.push_back(t);
            if (static_cast<int>(recent.size()) > a.window) {
                recent.erase(recent.begin(), recent.end() - a.window);
            }
            arrivals = recent;
        }
        if (recent.size() < 2) { std::fprintf(stderr, "no new messages\n"); continue; }
        std::vector<double> intervals;
        for (size_t i = 1; i < recent.size(); ++i) intervals.push_back(recent[i] - recent[i - 1]);
        double mean_iv = 0; for (double iv : intervals) mean_iv += iv; mean_iv /= intervals.size();
        double var = 0; for (double iv : intervals) var += (iv - mean_iv) * (iv - mean_iv); var /= intervals.size();
        double rate = recent.back() > recent.front() ? (recent.size() / (recent.back() - recent.front())) : 0.0;
        std::printf("average rate: %.3f\n", rate);
        std::printf("\tmin: %.5fs max: %.5fs std dev: %.5fs window: %zu\n",
                    *std::min_element(intervals.begin(), intervals.end()),
                    *std::max_element(intervals.begin(), intervals.end()),
                    std::sqrt(var), recent.size());
        std::fflush(stdout);
    }
}

static void cmd_bw(const Args& a) {
    RelinkNode node; configure_node(node, a);
    uint32_t topic_id = resolve_topic(node, a.topic);
    std::mutex m;
    std::vector<std::pair<double, size_t>> samples;
    node.subscribe_raw(topic_id, [&](const uint8_t*, size_t len) {
        std::lock_guard<std::mutex> lk(m);
        samples.push_back({std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count(), len});
    });
    node.spin_once();
    std::fprintf(stderr, "subscribed to topic id %u (%s), waiting for messages (Ctrl-C to stop)...\n",
                 topic_id, a.topic.c_str());
    std::signal(SIGINT, on_sigint);

    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        node.spin_once();
        std::vector<std::pair<double, size_t>> recent;
        {
            std::lock_guard<std::mutex> lk(m);
            double cutoff = std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch()).count() - a.report_every;
            for (auto& s : samples) if (s.first >= cutoff) recent.push_back(s);
            samples = recent;
        }
        if (recent.size() < 2) { std::fprintf(stderr, "no new messages\n"); continue; }
        size_t total = 0; for (auto& s : recent) total += s.second;
        double span = recent.back().first - recent.front().first;
        double bw = span > 0 ? total / span : 0.0;
        size_t mn = recent[0].second, mx = recent[0].second;
        for (auto& s : recent) { mn = std::min(mn, s.second); mx = std::max(mx, s.second); }
        std::printf("average: %.1f B/s\n", bw);
        std::printf("\tmean: %.1f B min: %zu B max: %zu B window: %zu\n",
                    double(total) / recent.size(), mn, mx, recent.size());
        std::fflush(stdout);
    }
}

static void cmd_echo(const Args& a) {
    RelinkNode node; configure_node(node, a);
    uint32_t topic_id = resolve_topic(node, a.topic);
    std::atomic<int> count{0};
    node.subscribe_raw(topic_id, [&](const uint8_t* payload, size_t len) {
        int n = ++count;
        std::printf("--- #%d (%zu bytes) ---\n", n, len);
        for (size_t i = 0; i < len; ++i) std::printf("%02x ", payload[i]);
        std::printf("\n");
        std::fflush(stdout);
        if (a.count && n >= a.count) std::_Exit(0); // same rationale as rl_topic.py: runs off the data thread
    });
    node.spin_once();
    std::fprintf(stderr, "subscribed to topic id %u (%s) (Ctrl-C to stop)...\n", topic_id, a.topic.c_str());
    std::signal(SIGINT, on_sigint);
    while (!g_stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

static void cmd_pub(const Args& a) {
    if (!a.have_hex && !a.have_text) {
        std::fprintf(stderr, "rl_topic pub: need --hex <hexbytes> or --text <string>\n");
        std::exit(2);
    }
    std::vector<uint8_t> payload;
    if (a.have_hex) {
        for (size_t i = 0; i + 1 < a.hex_payload.size(); i += 2) {
            payload.push_back(static_cast<uint8_t>(std::stoul(a.hex_payload.substr(i, 2), nullptr, 16)));
        }
    } else {
        payload.assign(a.text_payload.begin(), a.text_payload.end());
    }

    RelinkNode node; configure_node(node, a);
    uint32_t topic_id = resolve_topic(node, a.topic);
    node.advertise_raw(topic_id); // must declare BEFORE the first spin_once()

    auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(a.timeout);
    size_t n_peers = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        node.spin_once();
        n_peers = node.peers_for_topic(topic_id).size();
        if (n_peers > 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (n_peers == 0) {
        std::fprintf(stderr, "rl_topic pub: no peers found for topic id %u within %.1fs "
                     "-- publishing anyway (no-op if truly no one is listening)\n", topic_id, a.timeout);
    }

    int reps = a.repeat > 0 ? a.repeat : 1;
    for (int i = 0; i < reps; ++i) {
        bool ok = node.publish_raw(topic_id, payload.data(), payload.size());
        std::printf("published %zu bytes to topic id %u (%s): %s\n",
                    payload.size(), topic_id, a.topic.c_str(), ok ? "ok" : "no peers / failed");
        if (i + 1 < reps) std::this_thread::sleep_for(std::chrono::duration<double>(a.rate_period));
    }
}

int main(int argc, char** argv) {
    Args a = parse_args(argc, argv);
    if (a.command == "list") cmd_list(a);
    else if (a.command == "info") cmd_info(a);
    else if (a.command == "hz") cmd_hz(a);
    else if (a.command == "bw") cmd_bw(a);
    else if (a.command == "echo") cmd_echo(a);
    else if (a.command == "pub") cmd_pub(a);
    else {
        std::fprintf(stderr, "rl_topic: unknown command \"%s\" (list|info|hz|bw|echo|pub)\n", a.command.c_str());
        return 2;
    }
    return 0;
}

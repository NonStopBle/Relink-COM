// Small CLI used for cross-language interop testing of com-core: sends a
// RegisterRequest to a given com-core address/port with a given fake
// self ip:port and topic list, prints the resulting peers (or failure).
//
// usage: register_client_cli <server_ip> <server_port> <self_ip> <self_port> <topic_id> [more topic_ids...]

#include "relink/com_core_client.hpp"
#include "relink/udp_transport.hpp" // for ipv4_to_host_order
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace relink;

int main(int argc, char** argv) {
    if (argc < 6) {
        std::fprintf(stderr, "usage: %s <server_ip> <server_port> <self_ip> <self_port> <topic_id...>\n", argv[0]);
        return 2;
    }
    uint32_t server_ip = ipv4_to_host_order(argv[1]);
    uint16_t server_port = static_cast<uint16_t>(std::atoi(argv[2]));
    uint32_t self_ip = ipv4_to_host_order(argv[3]);
    uint16_t self_port = static_cast<uint16_t>(std::atoi(argv[4]));

    std::vector<uint16_t> topics;
    for (int i = 5; i < argc; ++i) topics.push_back(static_cast<uint16_t>(std::atoi(argv[i])));

    auto outcome = register_with_com_core(server_ip, server_port, self_ip, self_port,
                                           topics.data(), static_cast<uint16_t>(topics.size()));
    if (!outcome.ok) {
        std::printf("REGISTER_FAILED\n");
        return 1;
    }

    std::printf("REGISTER_OK peers=%zu\n", outcome.peers.size());
    for (const auto& p : outcome.peers) {
        struct in_addr ia{};
        ia.s_addr = htonl(p.ip);
        std::printf("  peer ip=%s port=%u topic=%u\n", inet_ntoa(ia), p.port, p.topic_id);
    }
    return 0;
}

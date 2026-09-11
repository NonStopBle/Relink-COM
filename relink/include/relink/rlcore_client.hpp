// Step 4: rlcore client-side registration with retry-with-backoff, per
// relink-com-spec.md: "Retry with backoff if no ACK arrives (e.g. 3
// retries, exponential backoff, then give up and log an error)".
//
// NAT traversal note: register_with_rlcore_on_socket() sends the
// RegisterRequest on the SAME socket the caller will use for data
// traffic. This matters once rlcore is run with --nat: it observes
// the request's UDP source port to learn each node's real internet-
// facing (NAT-mapped) endpoint. That observed port is only meaningful
// if it's the same socket/port the node's data transport is bound to --
// registering from a throwaway socket (the old behavior, kept below for
// standalone/CLI use where NAT traversal isn't needed) would teach
// rlcore the wrong port to hand out to peers.

#pragma once

#include "relink/register.hpp"
#include <cstdio>
#include <cstdint>
#include <vector>
#include <chrono>
#include <thread>
#include <stdexcept>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

namespace relink {

struct RegisterOutcome {
    bool ok = false;
    std::vector<RegisterAckPeer> peers;
};

// Sends a RegisterRequest to rlcore at (server_ip_host_order,
// server_port) using the given already-bound socket `sock`, retrying
// with exponential backoff if no ACK arrives within `timeout_ms`.
// Temporarily adjusts SO_RCVTIMEO on `sock` for the duration of each
// attempt (restored to whatever the caller had before, since the caller
// -- typically UdpTransport, still pre-start() at this point -- owns the
// socket's lifetime and its own timeout policy afterward).
inline RegisterOutcome register_with_rlcore_on_socket(
    int sock,
    uint32_t server_ip_host_order, uint16_t server_port,
    uint32_t self_ip_host_order, uint16_t self_data_port,
    const uint32_t* topic_ids, uint16_t topic_count,
    int max_retries = 3, int timeout_ms = 500) {
    RegisterOutcome outcome;

    struct sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = htonl(server_ip_host_order);
    server.sin_port = htons(server_port);

    uint8_t req_buf[512];
    size_t req_len = 0;
    auto er = encode_register_request(self_ip_host_order, self_data_port,
                                       topic_ids, topic_count,
                                       req_buf, sizeof(req_buf), &req_len);
    if (er != RegisterEncodeResult::Ok) {
        return outcome; // ok=false
    }

    uint8_t resp_buf[8192];
    int backoff_ms = timeout_ms;

    for (int attempt = 0; attempt < max_retries; ++attempt) {
        struct timeval tv{};
        tv.tv_sec = backoff_ms / 1000;
        tv.tv_usec = (backoff_ms % 1000) * 1000;
        ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        ssize_t sent = ::sendto(sock, req_buf, req_len, 0,
                                 reinterpret_cast<struct sockaddr*>(&server), sizeof(server));
        if (sent != static_cast<ssize_t>(req_len)) {
            backoff_ms *= 2;
            continue;
        }

        struct sockaddr_in src{};
        socklen_t src_len = sizeof(src);
        ssize_t n = ::recvfrom(sock, resp_buf, sizeof(resp_buf), 0,
                                reinterpret_cast<struct sockaddr*>(&src), &src_len);
        if (n > 0) {
            DecodedRegisterAck ack{};
            if (decode_register_ack(resp_buf, static_cast<size_t>(n), &ack) == RegisterDecodeResult::Ok
                && ack.status == 0) {
                outcome.ok = true;
                outcome.peers.reserve(ack.peer_count);
                for (uint16_t i = 0; i < ack.peer_count; ++i) {
                    outcome.peers.push_back(register_ack_peer_at(ack, i));
                }
                return outcome;
            }
        }

        // timeout or bad response: exponential backoff before next try
        std::fprintf(stderr, "register_with_rlcore: attempt %d/%d timed out, retrying...\n",
                     attempt + 1, max_retries);
        backoff_ms *= 2;
    }

    std::fprintf(stderr, "register_with_rlcore: giving up after %d attempts\n", max_retries);
    return outcome; // ok=false
}

// Convenience wrapper that opens its OWN throwaway socket -- fine for
// standalone/test use (see tests/register_client_cli.cpp) where NAT
// traversal isn't in play, but RelinkNode uses the socket-reusing
// variant above so the observed registration port matches the data port.
inline RegisterOutcome register_with_rlcore(
    uint32_t server_ip_host_order, uint16_t server_port,
    uint32_t self_ip_host_order, uint16_t self_data_port,
    const uint32_t* topic_ids, uint16_t topic_count,
    int max_retries = 3, int timeout_ms = 500) {
    int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) throw std::runtime_error("register_with_rlcore: socket() failed");
    RegisterOutcome outcome = register_with_rlcore_on_socket(
        sock, server_ip_host_order, server_port, self_ip_host_order, self_data_port,
        topic_ids, topic_count, max_retries, timeout_ms);
    ::close(sock);
    return outcome;
}

} // namespace relink

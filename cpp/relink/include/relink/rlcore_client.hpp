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
#include "relink/udp_transport.hpp"
#include "relink/platform.hpp"
#include "relink/crypto.hpp"
#include <cstdio>
#include <cstdint>
#include <vector>
#include <chrono>
#include <thread>
#include <stdexcept>

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
// `transport`: pass the owning UdpTransport when it may already have its
// dedicated recv thread running (i.e. this is a periodic re-registration
// call, not the initial pre-start() one). That thread permanently holds
// recvfrom() on this socket and wins the race for any incoming packet --
// including the RegisterAck this call is waiting for -- essentially
// every time, independent of traffic rate. When `transport` is running,
// wait on its handoff queue (get_register_reply()) instead of calling
// recvfrom() directly, closing that race. When `transport` is null or
// not yet started, recvfrom() here is safe (nothing else reads this
// socket yet).
//
// `encrypt_key`: when non-null (32 bytes, see crypto.hpp), the
// RegisterRequest is sealed with AES-256-GCM before sending and the
// RegisterAck is opened with the same key before decoding -- rlcore
// must be running with the matching --encrypt-key or every request
// from this node will be silently dropped as malformed on its side.
inline RegisterOutcome register_with_rlcore_on_socket(
    socket_t sock,
    uint32_t server_ip_host_order, uint16_t server_port,
    uint32_t self_ip_host_order, uint16_t self_data_port,
    const uint32_t* topic_ids, uint16_t topic_count,
    int max_retries = 3, int timeout_ms = 500,
    UdpTransport* transport = nullptr,
    const uint8_t* encrypt_key = nullptr) {
    RegisterOutcome outcome;

    struct sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = htonl(server_ip_host_order);
    server.sin_port = htons(server_port);

    // 65507 (largest possible UDP/IPv4 datagram), not a smaller fixed
    // size -- see the matching note on recv_buf_ in udp_transport.hpp
    // and on resp_buf below.
    uint8_t req_buf[65507];
    size_t req_len = 0;
    auto er = encode_register_request(self_ip_host_order, self_data_port,
                                       topic_ids, topic_count,
                                       req_buf, sizeof(req_buf), &req_len);
    if (er != RegisterEncodeResult::Ok) {
        // Was a silent ok=false with no message at all -- this failure
        // mode (topic_count over kMaxRegisterTopics, or the encoded
        // request not fitting req_buf) looked, from the caller, exactly
        // like "rlcore never replied", when actually no packet was ever
        // sent. Loud failure here matches every other failure path in
        // this function, which do print before giving up.
        std::fprintf(stderr, "register_with_rlcore: encode_register_request failed for %u topics "
                     "(result=%d) -- registration not attempted\n",
                     topic_count, static_cast<int>(er));
        return outcome; // ok=false
    }

    // Seal AFTER encoding but BEFORE the retry loop -- the same sealed
    // bytes are safe to resend verbatim on a timeout/retry (a fresh
    // random nonce per attempt would also be fine, but re-sealing the
    // identical plaintext on every retry is unnecessary work for no
    // benefit here).
    uint8_t sealed_req[65507];
    const uint8_t* wire_req = req_buf;
    size_t wire_req_len = req_len;
    if (encrypt_key != nullptr) {
        size_t sealed_len = 0;
        if (!aes256gcm_seal(encrypt_key, req_buf, req_len, sealed_req, sizeof(sealed_req), &sealed_len)) {
            std::fprintf(stderr, "register_with_rlcore: failed to encrypt RegisterRequest -- "
                         "registration not attempted\n");
            return outcome; // ok=false
        }
        wire_req = sealed_req;
        wire_req_len = sealed_len;
    }

    bool use_transport_queue = transport != nullptr && transport->is_running();
    // A RegisterAck listing many peers (large topic counts) can be
    // several KB -- an undersized buffer here silently truncates it at
    // the kernel level rather than erroring, corrupting a legitimate
    // reply into something decode_register_ack() rejects. Only used on
    // the direct-recvfrom() path; the transport-queue path receives
    // already-sized buffers from UdpTransport itself.
    uint8_t resp_buf[65507];
    int backoff_ms = timeout_ms;

    for (int attempt = 0; attempt < max_retries; ++attempt) {
        if (!use_transport_queue) {
            set_recv_timeout_ms(sock, backoff_ms);
        }

        ssize_t sent = static_cast<ssize_t>(::sendto(sock,
                                 reinterpret_cast<const char*>(wire_req), static_cast<int>(wire_req_len), 0,
                                 reinterpret_cast<struct sockaddr*>(&server), sizeof(server)));
        if (sent != static_cast<ssize_t>(wire_req_len)) {
            std::fprintf(stderr, "register_with_rlcore: attempt %d/%d timed out, retrying...\n",
                         attempt + 1, max_retries);
            backoff_ms *= 2;
            continue;
        }

        const uint8_t* resp_data = nullptr;
        size_t resp_len = 0;
        std::vector<uint8_t> queued;
        if (use_transport_queue) {
            if (transport->get_register_reply(&queued, backoff_ms)) {
                resp_data = queued.data();
                resp_len = queued.size();
            }
        } else {
            struct sockaddr_in src{};
            socklen_t src_len = sizeof(src);
            ssize_t n = static_cast<ssize_t>(::recvfrom(sock,
                                    reinterpret_cast<char*>(resp_buf), static_cast<int>(sizeof(resp_buf)), 0,
                                    reinterpret_cast<struct sockaddr*>(&src), &src_len));
            if (n > 0) {
                resp_data = resp_buf;
                resp_len = static_cast<size_t>(n);
            }
        }

        uint8_t opened_resp[65507];
        if (resp_data != nullptr && encrypt_key != nullptr) {
            size_t opened_len = 0;
            if (aes256gcm_open(encrypt_key, resp_data, resp_len, opened_resp, sizeof(opened_resp), &opened_len)) {
                resp_data = opened_resp;
                resp_len = opened_len;
            } else {
                std::fprintf(stderr, "register_with_rlcore: dropped RegisterAck that failed to decrypt "
                             "(wrong --encrypt-key on rlcore, or a corrupted/spoofed reply)\n");
                resp_data = nullptr;
            }
        }

        if (resp_data != nullptr) {
            DecodedRegisterAck ack{};
            if (decode_register_ack(resp_data, resp_len, &ack) == RegisterDecodeResult::Ok
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
    int max_retries = 3, int timeout_ms = 500,
    const uint8_t* encrypt_key = nullptr) {
    socket_t sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == kInvalidSocket) throw std::runtime_error("register_with_rlcore: socket() failed");
    RegisterOutcome outcome = register_with_rlcore_on_socket(
        sock, server_ip_host_order, server_port, self_ip_host_order, self_data_port,
        topic_ids, topic_count, max_retries, timeout_ms, nullptr, encrypt_key);
    relink::close_socket(sock);
    return outcome;
}

} // namespace relink

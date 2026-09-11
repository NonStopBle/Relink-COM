"""com-core client-side registration with retry-with-backoff -- mirrors
relink/include/relink/com_core_client.hpp. Per spec: "Retry with backoff
if no ACK arrives (e.g. 3 retries, exponential backoff, then give up and
log an error)."

NAT traversal note: register_with_com_core_on_socket() sends the
RegisterRequest on the SAME socket the caller will use for data traffic.
This matters once com-core is run with --nat: it observes the request's
UDP source port to learn each node's real internet-facing (NAT-mapped)
endpoint. That's only meaningful if it's the same socket/port the node's
data transport is bound to -- registering from a throwaway socket (the
old behavior, kept below as register_with_com_core() for standalone use
where NAT traversal isn't needed) would teach com-core the wrong port to
hand out to peers.
"""

import socket
import sys
from typing import List, NamedTuple, Optional

from .register import encode_register_request, decode_register_ack, RegisterAckPeer
from .udp_transport import host_order_to_ipv4


class RegisterOutcome(NamedTuple):
    ok: bool
    peers: List[RegisterAckPeer]


def register_with_com_core_on_socket(sock: socket.socket,
                                      server_ip_host_order: int, server_port: int,
                                      self_ip_host_order: int, self_data_port: int,
                                      topic_ids: List[int],
                                      max_retries: int = 3, timeout_s: float = 0.5) -> RegisterOutcome:
    request = encode_register_request(self_ip_host_order, self_data_port, topic_ids)
    server_addr = (host_order_to_ipv4(server_ip_host_order), server_port)

    # Save/restore the caller's timeout -- this socket belongs to
    # UdpTransport, which is typically still pre-start() here (no
    # dedicated thread reading it yet) but owns its own timeout policy
    # once that thread launches.
    original_timeout = sock.gettimeout()
    backoff = timeout_s
    try:
        for attempt in range(max_retries):
            sock.settimeout(backoff)
            try:
                sock.sendto(request, server_addr)
                data, _ = sock.recvfrom(8192)
                ack = decode_register_ack(data)
                if ack.status == 0:
                    return RegisterOutcome(True, ack.peers)
            except (socket.timeout, ValueError, OSError):
                pass

            print(f"register_with_com_core: attempt {attempt + 1}/{max_retries} timed out, retrying...",
                  file=sys.stderr)
            backoff *= 2

        print(f"register_with_com_core: giving up after {max_retries} attempts", file=sys.stderr)
        return RegisterOutcome(False, [])
    finally:
        sock.settimeout(original_timeout)


def register_with_com_core(server_ip_host_order: int, server_port: int,
                            self_ip_host_order: int, self_data_port: int,
                            topic_ids: List[int],
                            max_retries: int = 3, timeout_s: float = 0.5) -> RegisterOutcome:
    """Convenience wrapper that opens its OWN throwaway socket -- fine
    for standalone/test use where NAT traversal isn't in play. RelinkNode
    uses register_with_com_core_on_socket() with its transport's real
    socket instead."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        return register_with_com_core_on_socket(
            sock, server_ip_host_order, server_port, self_ip_host_order, self_data_port,
            topic_ids, max_retries, timeout_s)
    finally:
        sock.close()

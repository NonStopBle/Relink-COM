"""rlcore client-side registration with retry-with-backoff -- mirrors
relink/include/relink/rlcore_client.hpp. Per spec: "Retry with backoff
if no ACK arrives (e.g. 3 retries, exponential backoff, then give up and
log an error)."

NAT traversal note: register_with_rlcore_on_socket() sends the
RegisterRequest on the SAME socket the caller will use for data traffic.
This matters once rlcore is run with --nat: it observes the request's
UDP source port to learn each node's real internet-facing (NAT-mapped)
endpoint. That's only meaningful if it's the same socket/port the node's
data transport is bound to -- registering from a throwaway socket (the
old behavior, kept below as register_with_rlcore() for standalone use
where NAT traversal isn't needed) would teach rlcore the wrong port to
hand out to peers.
"""

import socket
import sys
from typing import List, NamedTuple, Optional, TYPE_CHECKING

from .register import encode_register_request, decode_register_ack, RegisterAckPeer
from .udp_transport import host_order_to_ipv4

if TYPE_CHECKING:
    from .udp_transport import UdpTransport


class RegisterOutcome(NamedTuple):
    ok: bool
    peers: List[RegisterAckPeer]


def register_with_rlcore_on_socket(sock: socket.socket,
                                      server_ip_host_order: int, server_port: int,
                                      self_ip_host_order: int, self_data_port: int,
                                      topic_ids: List[int],
                                      max_retries: int = 3, timeout_s: float = 0.5,
                                      transport: Optional["UdpTransport"] = None) -> RegisterOutcome:
    """`transport`: pass the owning UdpTransport when it may already have
    its dedicated recv thread running (i.e. this is a periodic
    re-registration call, not the initial pre-start() one). That thread
    permanently holds recvfrom() on this socket and wins the race for any
    incoming packet -- including the RegisterAck this call is waiting for
    -- essentially every time, independent of traffic rate (confirmed
    identical at 100Hz and 10000Hz). When `transport` is running, wait on
    its handoff queue instead of calling recvfrom() directly, closing that
    race. When `transport` is None or not yet started, recvfrom() here is
    safe (nothing else reads this socket yet)."""
    request = encode_register_request(self_ip_host_order, self_data_port, topic_ids)
    server_addr = (host_order_to_ipv4(server_ip_host_order), server_port)
    use_transport_queue = transport is not None and transport.is_running()

    # Save/restore the caller's timeout -- this socket belongs to
    # UdpTransport, which is typically still pre-start() here (no
    # dedicated thread reading it yet) but owns its own timeout policy
    # once that thread launches.
    original_timeout = sock.gettimeout()
    backoff = timeout_s
    try:
        for attempt in range(max_retries):
            if not use_transport_queue:
                sock.settimeout(backoff)
            try:
                sock.sendto(request, server_addr)
                if use_transport_queue:
                    data = transport.get_register_reply(backoff)
                    if data is None:
                        raise socket.timeout()
                else:
                    # 65507 (largest possible UDP/IPv4 datagram), not a
                    # smaller fixed size -- a RegisterAck listing many
                    # peers (large topic counts) can be several KB, and
                    # an undersized buffer here silently truncates it at
                    # the kernel level rather than erroring, corrupting
                    # a legitimate reply into something decode_register_ack()
                    # rejects. See the matching note in udp_transport.py's
                    # _recv_and_dispatch().
                    data, _ = sock.recvfrom(65507)
                ack = decode_register_ack(data)
                if ack.status == 0:
                    return RegisterOutcome(True, ack.peers)
            except (socket.timeout, ValueError, OSError):
                pass

            print(f"register_with_rlcore: attempt {attempt + 1}/{max_retries} timed out, retrying...",
                  file=sys.stderr)
            backoff *= 2

        print(f"register_with_rlcore: giving up after {max_retries} attempts", file=sys.stderr)
        return RegisterOutcome(False, [])
    finally:
        if not use_transport_queue:
            sock.settimeout(original_timeout)


def register_with_rlcore(server_ip_host_order: int, server_port: int,
                            self_ip_host_order: int, self_data_port: int,
                            topic_ids: List[int],
                            max_retries: int = 3, timeout_s: float = 0.5) -> RegisterOutcome:
    """Convenience wrapper that opens its OWN throwaway socket -- fine
    for standalone/test use where NAT traversal isn't in play. RelinkNode
    uses register_with_rlcore_on_socket() with its transport's real
    socket instead."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        return register_with_rlcore_on_socket(
            sock, server_ip_host_order, server_port, self_ip_host_order, self_data_port,
            topic_ids, max_retries, timeout_s)
    finally:
        sock.close()

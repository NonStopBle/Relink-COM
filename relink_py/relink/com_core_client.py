"""com-core client-side registration with retry-with-backoff -- mirrors
relink/include/relink/com_core_client.hpp. Per spec: "Retry with backoff
if no ACK arrives (e.g. 3 retries, exponential backoff, then give up and
log an error)."""

import socket
import sys
from typing import List, NamedTuple

from .register import encode_register_request, decode_register_ack, RegisterAckPeer
from .udp_transport import host_order_to_ipv4


class RegisterOutcome(NamedTuple):
    ok: bool
    peers: List[RegisterAckPeer]


def register_with_com_core(server_ip_host_order: int, server_port: int,
                            self_ip_host_order: int, self_data_port: int,
                            topic_ids: List[int],
                            max_retries: int = 3, timeout_s: float = 0.5) -> RegisterOutcome:
    request = encode_register_request(self_ip_host_order, self_data_port, topic_ids)
    server_addr = (host_order_to_ipv4(server_ip_host_order), server_port)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
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
        sock.close()

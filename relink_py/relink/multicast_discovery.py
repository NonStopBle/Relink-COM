"""One-shot jittered multicast beacon discovery -- mirrors
relink/include/relink/multicast_discovery.hpp. Sender thread does a 3x
jittered startup burst then sparse 30-60s re-announce; listener thread
matches incoming beacons' topics against the locally-declared set,
storing/updating the peer table on overlap and discarding immediately
(no state kept) on no overlap. Runs on its own threads, never sharing
the data path, per spec."""

import random
import socket
import struct
import threading
import time
from dataclasses import dataclass, field
from typing import Callable, Dict, List, Optional, Set, Tuple

from .beacon import encode_beacon_packet, decode_beacon_packet

DEFAULT_MULTICAST_GROUP = "239.255.0.1"
DEFAULT_MULTICAST_PORT = 7400


@dataclass
class PeerInfo:
    ip: int    # host byte order
    port: int


PeerDiscoveredCallback = Callable[[int, PeerInfo], None]


@dataclass
class MulticastDiscoveryConfig:
    group_ip: str = DEFAULT_MULTICAST_GROUP
    group_port: int = DEFAULT_MULTICAST_PORT
    self_ip: int = 0
    self_data_port: int = 0
    local_topics: List[int] = field(default_factory=list)
    startup_burst_count: int = 3
    startup_jitter_max_ms: int = 200
    reannounce_min_ms: int = 30000
    reannounce_max_ms: int = 60000


class MulticastDiscovery:
    def __init__(self, cfg: MulticastDiscoveryConfig):
        self._cfg = cfg
        self._local_topics: Set[int] = set(cfg.local_topics)
        self._running = threading.Event()
        self._send_sock: Optional[socket.socket] = None
        self._recv_sock: Optional[socket.socket] = None
        self._sender_thread: Optional[threading.Thread] = None
        self._listener_thread: Optional[threading.Thread] = None
        self._table_lock = threading.Lock()
        self._table: Dict[int, Set[Tuple[int, int]]] = {}
        self._on_peer_discovered: Optional[PeerDiscoveredCallback] = None

    def set_peer_discovered_callback(self, cb: PeerDiscoveredCallback):
        self._on_peer_discovered = cb

    def start(self):
        if self._running.is_set():
            return
        self._running.set()
        self._setup_send_socket()
        self._setup_recv_socket()
        self._sender_thread = threading.Thread(target=self._sender_loop, daemon=True)
        self._listener_thread = threading.Thread(target=self._listener_loop, daemon=True)
        self._sender_thread.start()
        self._listener_thread.start()

    def stop(self):
        if not self._running.is_set():
            return
        self._running.clear()
        if self._sender_thread is not None:
            self._sender_thread.join()
        if self._listener_thread is not None:
            self._listener_thread.join()
        if self._send_sock is not None:
            self._send_sock.close()
        if self._recv_sock is not None:
            self._recv_sock.close()

    def peers_for_topic(self, topic_id: int) -> List[PeerInfo]:
        with self._table_lock:
            return [PeerInfo(ip, port) for (ip, port) in self._table.get(topic_id, ())]

    def known_topic_count(self) -> int:
        with self._table_lock:
            return len(self._table)

    def _setup_send_socket(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
        s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 1)
        self._send_sock = s

    def _setup_recv_socket(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        if hasattr(socket, "SO_REUSEPORT"):
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
        s.bind(("", self._cfg.group_port))
        mreq = struct.pack("4sl", socket.inet_aton(self._cfg.group_ip), socket.INADDR_ANY)
        s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
        s.settimeout(0.05)
        self._recv_sock = s

    def _send_beacon_once(self):
        payload = encode_beacon_packet(self._cfg.self_ip, self._cfg.self_data_port, self._cfg.local_topics)
        try:
            self._send_sock.sendto(payload, (self._cfg.group_ip, self._cfg.group_port))
        except OSError:
            pass

    def _sleep_interruptible(self, total_ms: int):
        step_ms = 20
        slept = 0
        while slept < total_ms and self._running.is_set():
            chunk = min(step_ms, total_ms - slept)
            time.sleep(chunk / 1000.0)
            slept += chunk

    def _sender_loop(self):
        for i in range(self._cfg.startup_burst_count):
            if not self._running.is_set():
                return
            self._send_beacon_once()
            if i + 1 < self._cfg.startup_burst_count:
                self._sleep_interruptible(random.randint(0, self._cfg.startup_jitter_max_ms))

        while self._running.is_set():
            wait_ms = random.randint(self._cfg.reannounce_min_ms, self._cfg.reannounce_max_ms)
            self._sleep_interruptible(wait_ms)
            if not self._running.is_set():
                break
            self._send_beacon_once()

    def _listener_loop(self):
        while self._running.is_set():
            try:
                data, _addr = self._recv_sock.recvfrom(512)
            except socket.timeout:
                continue
            except OSError:
                continue

            try:
                beacon = decode_beacon_packet(data)
            except ValueError:
                continue  # malformed / non-ReLink traffic: drop

            if beacon.node_ip == self._cfg.self_ip and beacon.node_port == self._cfg.self_data_port:
                continue  # ignore our own beacon (loopback delivers it to us too)

            for topic in beacon.topic_ids:
                if topic not in self._local_topics:
                    continue  # no overlap: discard, keep no state

                key = (beacon.node_ip, beacon.node_port)
                with self._table_lock:
                    peer_set = self._table.setdefault(topic, set())
                    is_new = key not in peer_set
                    peer_set.add(key)
                if is_new and self._on_peer_discovered:
                    self._on_peer_discovered(topic, PeerInfo(*key))

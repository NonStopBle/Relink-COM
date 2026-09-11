"""The public ReLink API -- RelinkNode, mirrors relink/include/relink/relink.hpp.
Wires together the UDP data thread, com-core client, and multicast
discovery behind the same advertise/subscribe/publish<T> surface shown in
the C++ examples, adapted to Python idiom (pass the wire type as an
argument rather than a template parameter)."""

import ctypes
import enum
import socket
import struct
import threading
import time
from typing import Callable, Dict, List, Optional, Set, Type

from .wire import is_wire_type, NAT_PUNCH_TOPIC_ID
from .udp_transport import UdpTransport, PeerAddr, ipv4_to_host_order
from .com_core_client import register_with_com_core_on_socket
from .multicast_discovery import (
    MulticastDiscovery, MulticastDiscoveryConfig,
    DEFAULT_MULTICAST_GROUP, DEFAULT_MULTICAST_PORT,
)
from .register import COM_CORE_DEFAULT_PORT
from .image import (
    ImageChunk, encode_image_chunks, ImageReassembler, ImageTooLargeError,
    MAX_IMAGE_BYTES, IMAGE_CHUNK_DATA_BYTES, IMAGE_CHUNK_HEADER_BYTES,
)


class DiscoveryMode(enum.Enum):
    NONE = 0
    COM_CORE = 1
    MULTICAST = 2


def _detect_local_ip_for_peer(peer_ip_host_order: int, peer_port: int) -> int:
    """Same connect()+getsockname() trick as the C++ side: doesn't send
    any packet, just asks the kernel which local interface/address it
    would use to reach that peer, so the node can tell others where it
    is without the user hardcoding an interface."""
    from .udp_transport import host_order_to_ipv4
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((host_order_to_ipv4(peer_ip_host_order), peer_port))
        local_ip = s.getsockname()[0]
        return ipv4_to_host_order(local_ip)
    finally:
        s.close()


class _ComCoreConfig:
    """set_com_core sub-object: separate ip()/port() setters, port has a
    default, mode selection happens at the ip() call itself (not deferred
    to spin()/start())."""

    def __init__(self, owner: "RelinkNode"):
        self._owner = owner
        self._ip_set = False
        self._ip = 0
        self._port = COM_CORE_DEFAULT_PORT

    def ip(self, addr: str):
        self._owner._select_mode(DiscoveryMode.COM_CORE)
        self._ip = ipv4_to_host_order(addr)
        self._ip_set = True

    def port(self, p: int = COM_CORE_DEFAULT_PORT):
        self._port = p

    @property
    def ip_is_set(self) -> bool:
        return self._ip_set

    @property
    def resolved_ip(self) -> int:
        return self._ip

    @property
    def resolved_port(self) -> int:
        return self._port


class RelinkNode:
    def __init__(self):
        self.set_com_core = _ComCoreConfig(self)
        self._mode = DiscoveryMode.NONE
        self._declared_topics: Set[int] = set()
        self._peers_lock = threading.Lock()
        self._peers: Dict[int, List[PeerAddr]] = {}
        self._next_image_frame_id: Dict[int, int] = {}

        self._transport = UdpTransport()
        self._mcast: Optional[MulticastDiscovery] = None

        self._start_lock = threading.Lock()
        self._started = False
        self._stop_requested = threading.Event()

    # --- mode B, mutually exclusive with mode A ---
    def use_multicast_discovery(self):
        self._select_mode(DiscoveryMode.MULTICAST)

    def _select_mode(self, requested: DiscoveryMode):
        if self._mode != DiscoveryMode.NONE and self._mode != requested:
            raise RuntimeError(
                "discovery mode already set; cannot enable a second, mutually "
                "exclusive discovery mode on the same node")
        self._mode = requested

    # --- advertise: publisher-side topic declaration ---
    def advertise(self, topic_id: int, msg_type: Type[ctypes.Structure],
                  secure: bool = False, checksum: bool = False):
        if not is_wire_type(msg_type):
            raise TypeError(f"{msg_type} must be a ctypes.Structure subclass with _pack_ = 1")
        if msg_type is ImageChunk:
            self._transport.enable_large_buffers()
        self._declared_topics.add(topic_id)

    # --- subscribe: receiver-side topic declaration + typed callback,
    # invoked inline on the data thread, per spec's v1 threading design ---
    def subscribe(self, topic_id: int, msg_type: Type[ctypes.Structure],
                  callback: Callable[[ctypes.Structure], None], secure: bool = False):
        if not is_wire_type(msg_type):
            raise TypeError(f"{msg_type} must be a ctypes.Structure subclass with _pack_ = 1")
        self._declared_topics.add(topic_id)
        expected_size = ctypes.sizeof(msg_type)

        def raw_handler(payload: bytes):
            if len(payload) != expected_size:
                return  # type/size mismatch: drop, never misinterpret bytes
            callback(msg_type.from_buffer_copy(payload))

        self._transport.set_topic_handler(topic_id, raw_handler)

    # --- publish: sends to every currently-known peer for this topic ---
    def publish(self, topic_id: int, value: ctypes.Structure) -> bool:
        self._ensure_started()
        with self._peers_lock:
            peers = list(self._peers.get(topic_id, []))
        payload = bytes(value)
        all_ok = True
        for peer in peers:
            all_ok = self._transport.publish_raw(topic_id, payload, peer) and all_ok
        return all_ok

    # --- Image: a library-provided large-blob type, automatically
    # chunked to the MTU maximum on send and reassembled on receive.
    # See image.py -- built entirely on the same advertise/subscribe/
    # publish machinery above, one ordinary ImageChunk message per
    # datagram, not a new wire mechanism. Not JPEG/PNG-specific: carries
    # whatever bytes you give it. ---

    def advertise_image(self, topic_id: int):
        """Equivalent to advertise(topic_id, ImageChunk) -- a clearer
        name for this use case."""
        self.advertise(topic_id, ImageChunk)

    def publish_image(self, topic_id: int, data: bytes, frame_id: int = None) -> bool:
        """Splits data into MTU-maximized chunks and publishes each one
        in order. frame_id lets the receiver match chunks belonging to
        the same image and is auto-incremented per topic if not given.
        Raises ImageTooLargeError if data exceeds the max representable
        image size -- rejected loudly, never silently truncated.

        Zero-copy send path: unlike encode_image_chunks() (still
        available for the pure encode/reassemble unit tests and anyone
        building their own transport), this never builds an
        intermediate ImageChunk -- each chunk's small header and a
        `memoryview` slice of `data` are handed straight to
        UdpTransport.publish_scattered()'s socket.sendmsg(), so the
        chunk's actual pixel/compressed bytes are never copied at the
        Python level before being handed to the kernel."""
        if len(data) > MAX_IMAGE_BYTES:
            raise ImageTooLargeError(f"{len(data)} bytes exceeds the max representable image size "
                                      f"({MAX_IMAGE_BYTES} bytes, limited by chunk_count being a uint16)")
        if frame_id is None:
            with self._peers_lock:
                frame_id = self._next_image_frame_id.get(topic_id, 0)
                self._next_image_frame_id[topic_id] = (frame_id + 1) & 0xFFFFFFFF

        self._ensure_started()
        with self._peers_lock:
            peers = list(self._peers.get(topic_id, []))

        chunk_count = max(1, (len(data) + IMAGE_CHUNK_DATA_BYTES - 1) // IMAGE_CHUNK_DATA_BYTES)
        view = memoryview(data)
        all_ok = True
        for i in range(chunk_count):
            offset = i * IMAGE_CHUNK_DATA_BYTES
            piece = view[offset:offset + IMAGE_CHUNK_DATA_BYTES]
            header = struct.pack("<IHHH", frame_id, i, chunk_count, len(piece))
            for peer in peers:
                ok = self._transport.publish_scattered(topic_id, header, piece, peer)
                all_ok = ok and all_ok
        return all_ok

    def subscribe_image(self, topic_id: int, callback: Callable[[int, bytes], None]):
        """Subscribes to a topic of Image chunks; callback(frame_id,
        data) fires once per COMPLETE image (not once per chunk). An
        image whose chunks arrive incompletely before the next one
        starts is silently dropped -- no retransmission, matching
        ReLink's UDP design throughout (see image.py's ImageReassembler
        and examples/camera_stream.py's measured reliability numbers).

        IMPORTANT: like every subscribe() callback, this runs inline on
        ReLink's one data thread -- a slow callback (JPEG decode, disk
        I/O, ML inference) blocks recv() from draining the socket at
        all. The socket's larger receive buffer (see udp_transport.py)
        buys some slack for a short burst, but it is NOT a substitute
        for a fast callback: measured with a 50ms/frame callback against
        a faster publisher, per-frame latency grew linearly and delivery
        eventually collapsed once the backlog outran the buffer. If your
        work is slow, hand it off to your own worker thread/queue
        instead of doing it here."""
        self._transport.enable_large_buffers()
        reassembler = ImageReassembler(callback)
        self._declared_topics.add(topic_id)

        # Zero-copy receive path: publish_image() sends the compact wire
        # format (10-byte header + only the valid data bytes, not padded
        # to IMAGE_CHUNK_DATA_BYTES), so this parses that directly
        # instead of going through subscribe(..., ImageChunk, ...)
        # (which requires payload_len == ctypes.sizeof(ImageChunk)
        # exactly and would reject every partial last chunk).
        def raw_handler(payload: bytes):
            if len(payload) < IMAGE_CHUNK_HEADER_BYTES:
                return
            frame_id, chunk_index, chunk_count, chunk_bytes = struct.unpack(
                "<IHHH", payload[:IMAGE_CHUNK_HEADER_BYTES])
            data = payload[IMAGE_CHUNK_HEADER_BYTES:]
            if chunk_bytes != len(data):
                return  # mismatch: drop, never misinterpret bytes
            reassembler.on_chunk_raw(frame_id, chunk_index, chunk_count, data)

        self._transport.set_topic_handler(topic_id, raw_handler)

    def spin_once(self):
        self._ensure_started()

    def spin(self):
        self._ensure_started()
        while not self._stop_requested.is_set():
            time.sleep(0.02)

    def request_stop(self):
        self._stop_requested.set()

    def local_data_port(self) -> int:
        self._ensure_started()
        return self._transport.local_port

    def peers_for_topic(self, topic_id: int) -> List[PeerAddr]:
        with self._peers_lock:
            return list(self._peers.get(topic_id, []))

    def _ensure_started(self):
        with self._start_lock:
            if self._started:
                return

            if self._mode == DiscoveryMode.NONE:
                raise RuntimeError(
                    "no discovery method configured -- call set_com_core.ip(...) or "
                    "use_multicast_discovery() before spin()/publish()/subscribe traffic")
            if self._mode == DiscoveryMode.COM_CORE and not self.set_com_core.ip_is_set:
                raise RuntimeError("com-core IP not set -- call set_com_core.ip(...)")

            self._transport.bind(0)

            topics = list(self._declared_topics)
            newly_learned_peers: List[PeerAddr] = []  # for the NAT punch burst below

            if self._mode == DiscoveryMode.COM_CORE:
                # Registration MUST happen on the transport's own socket,
                # before self._transport.start() hands that socket's recv
                # loop to the dedicated data thread (two threads reading
                # the same fd concurrently would race the ack reply
                # against the data thread's dispatch loop). This also
                # matters for NAT traversal: when com-core runs with
                # --nat, it learns each node's real (NAT-mapped) public
                # endpoint from the request's UDP source port -- correct
                # only if that's the SAME port the node's data traffic
                # actually arrives on, i.e. this socket, not a throwaway
                # one.
                self_ip = _detect_local_ip_for_peer(self.set_com_core.resolved_ip,
                                                     self.set_com_core.resolved_port)
                outcome = register_with_com_core_on_socket(
                    self._transport.sock,
                    self.set_com_core.resolved_ip, self.set_com_core.resolved_port,
                    self_ip, self._transport.local_port, topics)
                if outcome.ok:
                    with self._peers_lock:
                        for p in outcome.peers:
                            addr = PeerAddr(p.ip, p.port)
                            self._peers.setdefault(p.topic_id, []).append(addr)
                            newly_learned_peers.append(addr)
                # If registration failed after retries, register_with_com_core
                # already logged an error; proceed with an empty peer table
                # rather than crashing the node.
                self._transport.start()
            else:
                self._transport.start()
                cfg = MulticastDiscoveryConfig(
                    self_ip=_detect_local_ip_for_peer(
                        ipv4_to_host_order(DEFAULT_MULTICAST_GROUP), DEFAULT_MULTICAST_PORT),
                    self_data_port=self._transport.local_port,
                    local_topics=topics,
                )
                self._mcast = MulticastDiscovery(cfg)

                def on_peer(topic: int, peer):
                    with self._peers_lock:
                        self._peers.setdefault(topic, []).append(PeerAddr(peer.ip, peer.port))

                self._mcast.set_peer_discovered_callback(on_peer)
                self._mcast.start()

            # NAT hole punching: fire a small burst of empty datagrams at
            # every peer learned from this registration. Only matters
            # (and is only correct) when com-core is run with --nat,
            # which hands out each peer's real internet-facing endpoint
            # instead of their self-reported LAN address -- sending a
            # datagram FROM this node TO that endpoint opens this node's
            # own NAT's outbound mapping so the peer's (simultaneous)
            # punch datagram back can get through. Harmless no-op cost
            # on a plain LAN.
            for peer in newly_learned_peers:
                for _ in range(3):
                    self._transport.publish_raw(NAT_PUNCH_TOPIC_ID, b"", peer)
                    time.sleep(0.03)

            self._started = True

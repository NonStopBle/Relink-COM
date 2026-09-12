"""The public ReLink API -- RelinkNode, mirrors relink/include/relink/relink.hpp.
Wires together the UDP data thread, rlcore client, and multicast
discovery behind the same advertise/subscribe/publish<T> surface shown in
the C++ examples, adapted to Python idiom (pass the wire type as an
argument rather than a template parameter)."""

import ctypes
import enum
import socket
import struct
import threading
import time
from typing import Callable, Dict, List, Optional, Set, Tuple, Type, Union

from .wire import is_wire_type, NAT_PUNCH_TOPIC_ID
from .topic_hash import fnv1a32
from .udp_transport import UdpTransport, PeerAddr, ipv4_to_host_order, host_order_to_ipv4
from .rlcore_client import register_with_rlcore_on_socket
from .multicast_discovery import (
    MulticastDiscovery, MulticastDiscoveryConfig, PortGroup,
    DEFAULT_MULTICAST_GROUP, DEFAULT_MULTICAST_PORT,
)
from .register import RLCORE_DEFAULT_PORT
from .topic_directory import TopicDirEntry
from . import topic_directory as tdir
from .relay_wire import RELAY_DEFAULT_PORT, encode_relay_register
from .image import (
    ImageChunk, encode_image_chunks, ImageReassembler, ImageTooLargeError,
    MAX_IMAGE_BYTES, IMAGE_CHUNK_DATA_BYTES, IMAGE_CHUNK_HEADER_BYTES,
)


class DiscoveryMode(enum.Enum):
    NONE = 0
    RLCORE = 1
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


class _RlCoreConfig:
    """set_rlcore sub-object: separate ip()/port() setters, port has a
    default, mode selection happens at the ip() call itself (not deferred
    to spin()/start())."""

    def __init__(self, owner: "RelinkNode"):
        self._owner = owner
        self._ip_set = False
        self._ip = 0
        self._port = RLCORE_DEFAULT_PORT

    def ip(self, addr: str):
        self._owner._select_mode(DiscoveryMode.RLCORE)
        self._ip = ipv4_to_host_order(addr)
        self._ip_set = True

    def port(self, p: int = RLCORE_DEFAULT_PORT):
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
        self.set_rlcore = _RlCoreConfig(self)
        self._mode = DiscoveryMode.NONE
        self._declared_topics: Set[int] = set()
        self._peers_lock = threading.Lock()
        self._peers: Dict[int, List[PeerAddr]] = {}
        self._next_image_frame_id: Dict[int, int] = {}
        self._topic_names: Dict[int, str] = {}

        self._transport = UdpTransport()
        self._mcast: Optional[MulticastDiscovery] = None
        self._multiplex = True
        self._topic_transports: Dict[int, UdpTransport] = {}
        self._topic_route: Dict[int, UdpTransport] = {}

        self._start_lock = threading.Lock()
        self._started = False
        self._stop_requested = threading.Event()

        self._repunch_enabled = False
        self._repunch_interval = 5.0
        self._repunch_stop = threading.Event()
        self._repunch_thread: Optional[threading.Thread] = None

        self._relay_enabled = False
        self._relay_ip = 0
        self._relay_port = RELAY_DEFAULT_PORT
        self._relay_stop = threading.Event()
        self._relay_thread: Optional[threading.Thread] = None

    # --- mode B, mutually exclusive with mode A ---
    def use_multicast_discovery(self):
        self._select_mode(DiscoveryMode.MULTICAST)

    def set_multiplex(self, enabled: bool):
        """True (default): every topic shares this node's one UDP
        socket/port. False: each advertise/subscribe/advertise_raw/
        subscribe_raw call after this is set gets its own dedicated
        UdpTransport on its own ephemeral port, ROS-style. Discovery
        automatically announces each topic's real port -- rlcore/
        multicast need no separate opt-in, since a registration/beacon
        already carries an explicit port alongside whichever topics it
        lists. Must be called before the advertise/subscribe/publish
        calls it should affect. Image stays on the shared transport
        regardless of this setting. See relink.hpp's set_multiplex()
        for the full rationale."""
        self._multiplex = enabled

    def enable_nat_repunch(self, interval_seconds: float = 5.0):
        """Opt-in: instead of firing the NAT hole-punch burst only once
        when a peer is first learned, keep re-punching every known peer
        on a timer for the node's whole lifetime. Fixes a marginal NAT
        whose mapping expires faster than expected, or a peer discovered
        just before the other side's mapping timed out. Does NOT fix a
        NAT/firewall that structurally drops all unsolicited inbound UDP
        regardless of timing (verified against a real mobile-carrier NAT:
        85 retries over 22 seconds still delivered zero packets) -- no
        amount of retrying opens a path that was never open. Call before
        spin()/publish() traffic; harmless no-op cost on a plain LAN,
        same as the one-shot burst it supplements. See relink.hpp's
        enable_nat_repunch() for the full rationale."""
        self._repunch_enabled = True
        self._repunch_interval = interval_seconds

    def _repunch_loop(self):
        while not self._repunch_stop.wait(self._repunch_interval):
            targets = []
            with self._peers_lock:
                for topic, peers in self._peers.items():
                    via = self._topic_route.get(topic, self._transport)
                    for p in peers:
                        targets.append((via, p))
            for via, p in targets:
                if self._repunch_stop.is_set():
                    break
                for _ in range(3):
                    via.publish_raw(NAT_PUNCH_TOPIC_ID, b"", p)
                    time.sleep(0.03)

    def set_relay(self, ip: str, port: int = RELAY_DEFAULT_PORT):
        """Relay fallback for when direct peer-to-peer hole punching
        cannot cross a NAT/firewall at all -- a real, verified failure
        mode (see README Step 13): some NATs (mobile carriers
        especially) drop unsolicited inbound UDP from a third party
        regardless of punch timing. A relay works there because both
        clients only ever open a NAT mapping toward the relay's one
        fixed (ip, port), never toward each other -- the relay's replies
        always come from that exact remote endpoint, which every
        stateful NAT/firewall allows back in, by definition.

        This is a redundant SECOND path, not a detect-failure-then-
        switch one: once enabled, every publish also goes to the relay,
        and every topic's socket also registers with (and is kept alive
        at) the relay, all the time -- direct punching still runs
        exactly as before. A subscriber that gets the same message from
        both paths silently drops the second copy (matched by seq_num,
        see UdpTransport.enable_relay_dedup()). Call before
        spin()/publish() traffic; must be paired with a relink-relay
        daemon running at `ip:port`. See relink.hpp's set_relay() for
        the full rationale."""
        self._relay_enabled = True
        self._relay_ip = ipv4_to_host_order(ip)
        self._relay_port = port

    def _relay_peer(self) -> PeerAddr:
        return PeerAddr(self._relay_ip, self._relay_port)

    def _register_all_topics_with_relay(self, groups):
        # (Dedup is armed earlier, in _ensure_started(), before any
        # transport.start() call -- see that call site for why the
        # ordering matters.)
        for t, group_topics in groups:
            for topic in group_topics:
                pkt = encode_relay_register(topic)
                try:
                    t.sock.sendto(pkt, (host_order_to_ipv4(self._relay_ip), self._relay_port))
                except OSError:
                    pass

    def _relay_keepalive_loop(self, groups):
        # Must outpace the relay daemon's MEMBER_TTL_SECONDS (30s) by a
        # comfortable margin so a scheduling hiccup doesn't drop this
        # node out of a topic's forwarding group.
        while not self._relay_stop.wait(10.0):
            self._register_all_topics_with_relay(groups)

    def _transport_for(self, topic_id: int) -> UdpTransport:
        if self._multiplex:
            return self._transport
        t = self._topic_transports.get(topic_id)
        if t is None:
            t = UdpTransport()
            t.bind(0)
            self._topic_transports[topic_id] = t
        return t

    def _select_mode(self, requested: DiscoveryMode):
        if self._mode != DiscoveryMode.NONE and self._mode != requested:
            raise RuntimeError(
                "discovery mode already set; cannot enable a second, mutually "
                "exclusive discovery mode on the same node")
        self._mode = requested

    # --- named topics: hash a human-readable topic name down to the
    # uint32 that actually goes on the wire. One-way (FNV-1a) -- see
    # topic_hash.py for why this can't be losslessly inverted.
    # "Decoding" an id back to a name only works locally via the
    # registry this populates (topic_name_for()), for names this
    # process itself has advertised/subscribed/published.
    def _topic_id_for(self, topic: Union[int, str]) -> int:
        if not isinstance(topic, str):
            return topic
        topic_id = fnv1a32(topic)
        if topic_id == NAT_PUNCH_TOPIC_ID:
            raise RuntimeError(
                f'topic name "{topic}" hashes to the reserved NAT-punch topic id -- '
                "pick a different name")
        existing = self._topic_names.get(topic_id)
        if existing is not None:
            if existing != topic:
                raise RuntimeError(
                    f'topic hash collision between "{existing}" and "{topic}" '
                    f"(both hash to {topic_id})")
        else:
            self._topic_names[topic_id] = topic
        return topic_id

    def topic_name_for(self, topic_id: int) -> str:
        """Reverse lookup into this process's own registry -- empty
        string if this id was never named here."""
        return self._topic_names.get(topic_id, "")

    def rltopic_list(self) -> List[Dict[str, object]]:
        """Lists every topic id THIS node has itself declared, PLUS --
        when using multicast discovery -- every topic id any other
        node's beacon has announced, network-wide (rostopic-list-style).
        Beacons only ever carry the numeric id, never a name, so a topic
        learned purely from the network has name "" here; it only gets a
        name if this same process separately resolved that id itself via
        _topic_id_for() (i.e. it also advertised/subscribed that name).
        Mode A (rlcore) does not currently feed this beyond what this
        node declared -- the daemon doesn't broadcast a topic roster."""
        ids = set(self._declared_topics)
        if self._mcast is not None:
            ids.update(self._mcast.all_known_topic_ids())
        return [
            {"topic_id": tid, "name": self._topic_names.get(tid, "")}
            for tid in sorted(ids)
        ]

    # --- advertise: publisher-side topic declaration ---
    def advertise(self, topic: Union[int, str], msg_type: Type[ctypes.Structure],
                  secure: bool = False, checksum: bool = False):
        topic_id = self._topic_id_for(topic)
        if not is_wire_type(msg_type):
            raise TypeError(f"{msg_type} must be a ctypes.Structure subclass with _pack_ = 1")
        if msg_type is ImageChunk:
            self._transport.enable_large_buffers()  # Image always stays on the shared transport
        else:
            self._transport_for(topic_id)
        self._declared_topics.add(topic_id)

    # --- subscribe: receiver-side topic declaration + typed callback,
    # invoked inline on the data thread, per spec's v1 threading design ---
    def subscribe(self, topic: Union[int, str], msg_type: Type[ctypes.Structure],
                  callback: Callable[[ctypes.Structure], None], secure: bool = False):
        topic_id = self._topic_id_for(topic)
        if not is_wire_type(msg_type):
            raise TypeError(f"{msg_type} must be a ctypes.Structure subclass with _pack_ = 1")
        self._declared_topics.add(topic_id)
        expected_size = ctypes.sizeof(msg_type)
        t = self._transport if msg_type is ImageChunk else self._transport_for(topic_id)

        def raw_handler(payload: bytes):
            if len(payload) != expected_size:
                return  # type/size mismatch: drop, never misinterpret bytes
            callback(msg_type.from_buffer_copy(payload))

        t.set_topic_handler(topic_id, raw_handler)

    # --- subscribe_raw/publish_raw: type-agnostic escape hatch, for
    # tooling that inspects a topic without knowing its message type
    # (rl_topic.py's echo/hz/bw subcommands) -- NOT for application
    # code, which should always use the typed advertise/subscribe/
    # publish above so a size mismatch is caught per ReLink's "never
    # misinterpret bytes" rule instead of being handed unstructured
    # bytes.
    def advertise_raw(self, topic: Union[int, str]):
        """Declares intent to publish `topic` without committing to a
        message type -- must be called (or subscribe_raw/publish_raw
        called) BEFORE the first spin_once()/publish()/subscribe() of
        any kind, same ordering requirement as advertise<T>, since the
        topic list beacons/registers with is snapshotted once at
        ensure_started() time."""
        topic_id = self._topic_id_for(topic)
        self._transport_for(topic_id)
        self._declared_topics.add(topic_id)

    def subscribe_raw(self, topic: Union[int, str], callback: Callable[[bytes], None]):
        topic_id = self._topic_id_for(topic)
        t = self._transport_for(topic_id)
        self._declared_topics.add(topic_id)
        t.set_topic_handler(topic_id, callback)

    def publish_raw(self, topic: Union[int, str], payload: bytes) -> bool:
        topic_id = self._topic_id_for(topic)
        self._declared_topics.add(topic_id)
        self._ensure_started()
        with self._peers_lock:
            peers = list(self._peers.get(topic_id, []))
        t = self._transport_for(topic_id)
        seq = t.next_seq()  # shared across every direct peer AND the relay copy
        all_ok = True
        for peer in peers:
            all_ok = t.publish_raw(topic_id, payload, peer, seq) and all_ok
        if self._relay_enabled:
            t.publish_raw(topic_id, payload, self._relay_peer(), seq)
        return all_ok and bool(peers)

    # --- publish: sends to every currently-known peer for this topic ---
    def publish(self, topic: Union[int, str], value: ctypes.Structure) -> bool:
        topic_id = self._topic_id_for(topic)
        self._ensure_started()
        with self._peers_lock:
            peers = list(self._peers.get(topic_id, []))
        payload = bytes(value)
        t = self._transport if type(value) is ImageChunk else self._transport_for(topic_id)
        seq = t.next_seq()  # shared across every direct peer AND the relay copy
        all_ok = True
        for peer in peers:
            all_ok = t.publish_raw(topic_id, payload, peer, seq) and all_ok
        if self._relay_enabled and type(value) is not ImageChunk:
            t.publish_raw(topic_id, payload, self._relay_peer(), seq)
        return all_ok

    # --- Image: a library-provided large-blob type, automatically
    # chunked to the MTU maximum on send and reassembled on receive.
    # See image.py -- built entirely on the same advertise/subscribe/
    # publish machinery above, one ordinary ImageChunk message per
    # datagram, not a new wire mechanism. Not JPEG/PNG-specific: carries
    # whatever bytes you give it. ---

    def advertise_image(self, topic: Union[int, str]):
        """Equivalent to advertise(topic, ImageChunk) -- a clearer
        name for this use case."""
        self.advertise(topic, ImageChunk)

    def publish_image(self, topic: Union[int, str], data: bytes, frame_id: int = None) -> bool:
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
        topic_id = self._topic_id_for(topic)
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

    def subscribe_image(self, topic: Union[int, str], callback: Callable[[int, bytes], None]):
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
        topic_id = self._topic_id_for(topic)
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
                    "no discovery method configured -- call set_rlcore.ip(...) or "
                    "use_multicast_discovery() before spin()/publish()/subscribe traffic")
            if self._mode == DiscoveryMode.RLCORE and not self.set_rlcore.ip_is_set:
                raise RuntimeError("rlcore IP not set -- call set_rlcore.ip(...)")

            self._transport.bind(0)

            topics = list(self._declared_topics)

            # One group per distinct transport a declared topic ended up
            # on: one group (the shared transport) in the default
            # multiplexed mode; one group per topic (plus a leftover
            # group for anything that stayed on the shared transport,
            # e.g. Image) when set_multiplex(False) is in effect. This is
            # the only place multiplex vs. demultiplex changes discovery
            # behavior -- rlcore/multicast don't need to know which mode
            # a node is in, since each registration/beacon already
            # carries its own explicit port alongside whichever topics it
            # lists.
            groups = []  # list of (transport, topics)
            if not self._multiplex and self._topic_transports:
                demuxed = set(self._topic_transports.keys())
                for tid, t in self._topic_transports.items():
                    groups.append((t, [tid]))
                leftover = [tid for tid in self._declared_topics if tid not in demuxed]
                if leftover:
                    groups.append((self._transport, leftover))
            elif topics:
                groups.append((self._transport, topics))

            # Which transport owns each topic's data socket, so NAT
            # hole-punching fires from the SAME socket that topic's
            # traffic actually uses -- required once set_multiplex(False)
            # gives each topic its own port/NAT mapping, since punching
            # from the wrong socket opens the wrong mapping and the
            # peer's simultaneous punch back never gets through.
            for t, group_topics in groups:
                for tid in group_topics:
                    self._topic_route[tid] = t

            # Dedup MUST be armed before any transport.start() call below
            # launches its recv thread, not after -- enabling it post-
            # start leaves a race window where an early direct+relay
            # duplicate pair can both slip through before the flag takes
            # effect.
            if self._relay_enabled:
                for t, _ in groups:
                    t.enable_relay_dedup()

            newly_learned_peers: List[Tuple[UdpTransport, PeerAddr]] = []  # for the NAT punch burst below

            if self._mode == DiscoveryMode.RLCORE:
                # Registration MUST happen on the transport's own socket,
                # before self._transport.start() hands that socket's recv
                # loop to the dedicated data thread (two threads reading
                # the same fd concurrently would race the ack reply
                # against the data thread's dispatch loop). This also
                # matters for NAT traversal: when rlcore runs with
                # --nat, it learns each node's real (NAT-mapped) public
                # endpoint from the request's UDP source port -- correct
                # only if that's the SAME port the node's data traffic
                # actually arrives on, i.e. this socket, not a throwaway
                # one.
                self_ip = _detect_local_ip_for_peer(self.set_rlcore.resolved_ip,
                                                     self.set_rlcore.resolved_port)
                for t, group_topics in groups:
                    outcome = register_with_rlcore_on_socket(
                        t.sock,
                        self.set_rlcore.resolved_ip, self.set_rlcore.resolved_port,
                        self_ip, t.local_port, group_topics)
                    if outcome.ok:
                        with self._peers_lock:
                            for p in outcome.peers:
                                addr = PeerAddr(p.ip, p.port)
                                self._peers.setdefault(p.topic_id, []).append(addr)
                                newly_learned_peers.append((t, addr))
                # If registration failed after retries, register_with_rlcore
                # already logged an error; proceed with an empty peer table
                # rather than crashing the node.

                # Tell rlcore about any names we resolved for these topics
                # (best-effort, fire-and-forget -- see the C++ side's
                # identical comment in relink.hpp for the rationale).
                if self._topic_names:
                    entries = [TopicDirEntry(tid, name) for tid, name in self._topic_names.items()]
                    try:
                        announce = tdir.encode_announce(entries)
                        self._transport.sock.sendto(
                            announce, (host_order_to_ipv4(self.set_rlcore.resolved_ip), self.set_rlcore.resolved_port))
                    except (ValueError, OSError):
                        pass

                self._transport.start()
            else:
                self._transport.start()
                cfg = MulticastDiscoveryConfig(
                    self_ip=_detect_local_ip_for_peer(
                        ipv4_to_host_order(DEFAULT_MULTICAST_GROUP), DEFAULT_MULTICAST_PORT),
                    port_groups=[PortGroup(t.local_port, group_topics) for t, group_topics in groups],
                )
                self._mcast = MulticastDiscovery(cfg)

                def on_peer(topic: int, peer):
                    addr = PeerAddr(peer.ip, peer.port)
                    with self._peers_lock:
                        self._peers.setdefault(topic, []).append(addr)
                        via = self._topic_route.get(topic, self._transport)
                    # Multicast discovery keeps running for the node's
                    # whole lifetime (unlike rlcore's one-shot registration
                    # burst below), so a peer for a set_multiplex(False)
                    # topic can show up long after start() -- punch from
                    # that topic's OWN socket every time, not just at
                    # startup, or its NAT mapping never opens and the
                    # peer's punch back is dropped.
                    for _ in range(3):
                        via.publish_raw(NAT_PUNCH_TOPIC_ID, b"", addr)
                        time.sleep(0.03)

                self._mcast.set_peer_discovered_callback(on_peer)

                def name_provider():
                    return [TopicDirEntry(tid, name) for tid, name in self._topic_names.items()]

                self._mcast.set_topic_name_provider(name_provider)
                self._mcast.start()

            # Demultiplexed topics each need their own data thread too --
            # set_topic_handler() was already called on these at
            # advertise/subscribe time, before this point.
            for t in self._topic_transports.values():
                t.start()

            # NAT hole punching: fire a small burst of empty datagrams at
            # every peer learned from this registration. Only matters
            # (and is only correct) when rlcore is run with --nat,
            # which hands out each peer's real internet-facing endpoint
            # instead of their self-reported LAN address -- sending a
            # datagram FROM this node TO that endpoint opens this node's
            # own NAT's outbound mapping so the peer's (simultaneous)
            # punch datagram back can get through. Harmless no-op cost
            # on a plain LAN.
            for t, peer in newly_learned_peers:
                for _ in range(3):
                    t.publish_raw(NAT_PUNCH_TOPIC_ID, b"", peer)
                    time.sleep(0.03)

            # NAT mode (rlcore, the only mode --nat applies to) gets a 1s
            # re-punch thread by default -- this is the mode where a
            # peer's real endpoint was learned from a NAT-mapped source
            # port that can drift/expire, so continuously rechecking
            # readiness matters enough to not require an opt-in call. A
            # user who already called enable_nat_repunch() themselves
            # (any mode, any interval) keeps their own setting -- this
            # only fills in the default when nothing was requested.
            if self._mode == DiscoveryMode.RLCORE and not self._repunch_enabled:
                self._repunch_enabled = True
                self._repunch_interval = 1.0

            if self._repunch_enabled:
                self._repunch_thread = threading.Thread(target=self._repunch_loop, daemon=True)
                self._repunch_thread.start()

            if self._relay_enabled:
                self._register_all_topics_with_relay(groups)  # immediate, don't wait for the first keepalive tick
                self._relay_thread = threading.Thread(target=self._relay_keepalive_loop, args=(groups,), daemon=True)
                self._relay_thread.start()

            self._started = True

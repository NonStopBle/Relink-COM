"""Raw UDP send/receive core on its own dedicated data thread -- mirrors
relink/include/relink/udp_transport.hpp, per relink-com-spec.md's
Threading section. v1 scope: secure=false, checksum=false only.

Python can't give the same zero-heap-allocation guarantee the C++ side
does (the interpreter allocates for every bytes object), so this binding
does not claim the 1000Hz hard requirement -- it's provided for
interoperability and convenience (tooling, test scripts, non-hot-path
nodes), matching the spec's framing of "hand-written bindings per
language are fine for v1," not a second implementation racing the C++
core for the performance target.
"""

import collections
import socket
import struct
import threading
from typing import Callable, Dict, NamedTuple, Optional

from .frame import encode_frame, decode_frame, MAX_FRAME_BYTES, FrameError


class PeerAddr(NamedTuple):
    ip_host_order: int
    port: int


def ipv4_to_host_order(dotted: str) -> int:
    return struct.unpack("!I", socket.inet_aton(dotted))[0]


def host_order_to_ipv4(value: int) -> str:
    return socket.inet_ntoa(struct.pack("!I", value & 0xFFFFFFFF))


RawTopicCallback = Callable[[bytes], None]


class UdpTransport:
    def __init__(self):
        self._sock: Optional[socket.socket] = None
        self._local_port = 0
        self._running = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self._handlers_lock = threading.Lock()
        self._handlers: Dict[int, RawTopicCallback] = {}
        self._seq = 0
        self._seq_lock = threading.Lock()
        self._relay_dedup_enabled = False
        # Remembers the last few delivered seq_nums per topic, not just
        # the most recent one: a relay copy can legitimately arrive
        # AFTER several newer direct messages already advanced past it
        # (the relay hop adds real latency), so "only equal to the
        # single last one" would miss it. A small fixed-size deque
        # (membership checked by linear scan -- cheap at this size)
        # catches a duplicate arriving reasonably out of order.
        self._recent_seqs: Dict[int, "collections.deque"] = {}  # topic_id -> deque[seq_num]
        self._want_large_buffers = False

    def enable_large_buffers(self):
        """Opts this node's socket into a larger send/receive buffer.
        Only advertise(..., ImageChunk)/subscribe(..., ImageChunk) call
        this -- a plain small-message node (the common, latency-
        sensitive case this library is built for, measured ~3x faster
        than ROS2 Humble) keeps the OS default buffer untouched. A
        bigger buffer is not free: more memory reserved per socket and,
        under sustained overload, more queued backlog before a drop
        finally happens (worse latency, not better) -- so it's only
        requested for the one message type actually measured to need
        it. Sized to the lowest value that measured reliably: a
        20-frame, 5 FPS, 640x480 raw burst (664 chunks/frame, ~930KB/
        frame) delivered 20/20 at 512KB-1MB repeatedly. Safe to call
        before or after bind()."""
        self._want_large_buffers = True
        if self._sock is not None:
            self._apply_large_buffers()

    def _apply_large_buffers(self):
        try:
            self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024 * 1024)
            self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1024 * 1024)
        except OSError:
            pass

    def bind(self, port: int = 0):
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        # Bounded recv timeout so the loop can check the stop flag
        # promptly instead of blocking forever -- same rationale as the
        # C++ side's SO_RCVTIMEO.
        self._sock.settimeout(0.05)
        # This is the ONE socket the node uses for every topic; see
        # enable_large_buffers() above for why this is opt-in, not
        # applied unconditionally to every node.
        if self._want_large_buffers:
            self._apply_large_buffers()
        self._sock.bind(("0.0.0.0", port))
        self._local_port = self._sock.getsockname()[1]

    @property
    def local_port(self) -> int:
        return self._local_port

    @property
    def sock(self) -> socket.socket:
        """The underlying socket, exposed ONLY so a caller (namely
        RelinkNode's rlcore registration step) can reuse this exact
        socket for a pre-start() synchronous request/reply, keeping the
        NAT-mapped source port rlcore observes consistent with the
        port this transport will actually receive data on. Safe to use
        before start() launches the dedicated data thread; not meant for
        general use once that thread owns recv from this socket."""
        return self._sock

    def set_topic_handler(self, topic_id: int, callback: RawTopicCallback):
        with self._handlers_lock:
            self._handlers[topic_id] = callback

    def next_seq(self) -> int:
        """Draws a fresh seq_num without sending -- so a caller that will
        send the SAME logical message to several destinations (e.g.
        every direct peer, plus a relay fallback copy) can give them all
        the same seq_num via publish_raw's `seq` parameter. That's what
        lets a relay-dedup-enabled receiver recognize direct and relayed
        copies of one publish() call as the same message rather than
        two."""
        with self._seq_lock:
            seq = self._seq
            self._seq = (self._seq + 1) & 0xFFFF
            return seq

    def publish_raw(self, topic_id: int, payload: bytes, peer: PeerAddr, seq: Optional[int] = None) -> bool:
        if seq is None:
            seq = self.next_seq()
        try:
            frame = encode_frame(topic_id, seq, payload)
        except FrameError:
            return False
        try:
            self._sock.sendto(frame, (host_order_to_ipv4(peer.ip_host_order), peer.port))
            return True
        except OSError:
            return False

    def enable_relay_dedup(self):
        """Opts this transport into dropping an exact-duplicate seq_num
        per topic before it reaches the subscriber callback. Off by
        default -- only relay fallback turns this on, since that's the
        one case where the SAME message can legitimately arrive twice on
        the same socket (once direct, once relayed). See relink.hpp's
        enable_relay_dedup() for the full rationale."""
        self._relay_dedup_enabled = True

    _RECENT_SEQ_WINDOW = 8

    def _is_recent_duplicate(self, topic_id: int, seq: int) -> bool:
        window = self._recent_seqs.get(topic_id)
        if window is None:
            window = collections.deque(maxlen=self._RECENT_SEQ_WINDOW)
            self._recent_seqs[topic_id] = window
        if seq in window:
            return True
        window.append(seq)
        return False

    def publish_scattered(self, topic_id: int, extra_header: bytes,
                           data, peer: PeerAddr) -> bool:
        """Zero-copy variant for large-blob types like Image: publish_raw()
        above builds one joined bytes object (start + header + payload +
        stop) before sending, which for Image means an extra Python-level
        copy of the caller's chunk data on top of the encode/dispatch
        overhead already inherent to the interpreter. This uses
        socket.sendmsg() with a scatter-gather buffer list instead, so
        `data` (the caller's own buffer, e.g. a `memoryview` slice of a
        camera frame -- pass one to avoid a `bytes` slice copy too) is
        handed to the kernel directly alongside the small fixed framing
        pieces, without ever being concatenated into one object."""
        payload_len = len(extra_header) + len(data)
        from .frame import MAX_PAYLOAD_BYTES
        from .wire import RelinkHeader, START_BYTE, STOP_BYTE
        if payload_len > MAX_PAYLOAD_BYTES:
            return False
        with self._seq_lock:
            seq = self._seq
            self._seq = (self._seq + 1) & 0xFFFF
        header = bytes(RelinkHeader(topic_id=topic_id, seq_num=seq,
                                     payload_len=payload_len, flags=0))
        buffers = [bytes([START_BYTE]), header]
        if extra_header:
            buffers.append(extra_header)
        if len(data):
            buffers.append(data)
        buffers.append(bytes([STOP_BYTE]))
        try:
            sent = self._sock.sendmsg(buffers, [], 0,
                                       (host_order_to_ipv4(peer.ip_host_order), peer.port))
            return sent == 1 + len(header) + payload_len + 1
        except OSError:
            return False

    def start(self):
        if self._running.is_set():
            return
        self._running.set()
        self._thread = threading.Thread(target=self._run_loop, daemon=True)
        self._thread.start()

    def stop(self):
        if not self._running.is_set():
            return
        self._running.clear()
        if self._thread is not None:
            self._thread.join()
        if self._sock is not None:
            self._sock.close()
            self._sock = None

    def _run_loop(self):
        while self._running.is_set():
            self._recv_and_dispatch()

    def _recv_and_dispatch(self) -> bool:
        try:
            data, _addr = self._sock.recvfrom(MAX_FRAME_BYTES)
        except socket.timeout:
            return False
        except OSError:
            return False

        try:
            frame = decode_frame(data)
        except FrameError:
            return False  # drop silently, per spec

        if self._relay_dedup_enabled and self._is_recent_duplicate(frame.topic_id, frame.seq_num):
            return True  # exact duplicate (direct + relay both delivered it) -- drop

        with self._handlers_lock:
            handler = self._handlers.get(frame.topic_id)
        if handler is not None:
            handler(frame.payload)  # inline on the data thread, per spec
            return True
        return False

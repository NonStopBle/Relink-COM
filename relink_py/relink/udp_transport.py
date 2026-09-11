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

    def bind(self, port: int = 0):
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        # Bounded recv timeout so the loop can check the stop flag
        # promptly instead of blocking forever -- same rationale as the
        # C++ side's SO_RCVTIMEO.
        self._sock.settimeout(0.05)
        # A multi-chunk Image burst (advertise_image/publish_image) can
        # land hundreds of datagrams back-to-back faster than Python's
        # per-chunk recv+dispatch overhead can drain them; the OS default
        # SO_RCVBUF overflows well before a several-hundred-chunk burst is
        # drained, silently dropping the tail (a dropped chunk drops the
        # whole image -- Image never retransmits). Request a much larger
        # buffer so a full burst fits in the kernel queue; best-effort --
        # if the OS clamps it, that's fine, this is strictly better than
        # the default, never worse.
        try:
            self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
            self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4 * 1024 * 1024)
        except OSError:
            pass
        self._sock.bind(("0.0.0.0", port))
        self._local_port = self._sock.getsockname()[1]

    @property
    def local_port(self) -> int:
        return self._local_port

    @property
    def sock(self) -> socket.socket:
        """The underlying socket, exposed ONLY so a caller (namely
        RelinkNode's com-core registration step) can reuse this exact
        socket for a pre-start() synchronous request/reply, keeping the
        NAT-mapped source port com-core observes consistent with the
        port this transport will actually receive data on. Safe to use
        before start() launches the dedicated data thread; not meant for
        general use once that thread owns recv from this socket."""
        return self._sock

    def set_topic_handler(self, topic_id: int, callback: RawTopicCallback):
        with self._handlers_lock:
            self._handlers[topic_id] = callback

    def publish_raw(self, topic_id: int, payload: bytes, peer: PeerAddr) -> bool:
        with self._seq_lock:
            seq = self._seq
            self._seq = (self._seq + 1) & 0xFFFF
        try:
            frame = encode_frame(topic_id, seq, payload)
        except FrameError:
            return False
        try:
            self._sock.sendto(frame, (host_order_to_ipv4(peer.ip_host_order), peer.port))
            return True
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

        with self._handlers_lock:
            handler = self._handlers.get(frame.topic_id)
        if handler is not None:
            handler(frame.payload)  # inline on the data thread, per spec
            return True
        return False

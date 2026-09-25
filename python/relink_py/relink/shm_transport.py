"""Same-host IPC transport: a lock-free SPSC shared-memory ring buffer,
used by RelinkNode's advertise_local_ipc/subscribe_local_ipc/
publish_local_ipc as an explicit opt-in alternative to UDP for two
processes known to be on the same machine. Mirrors
relink/include/relink/shm_transport.hpp byte-for-byte -- a Python and
C++ process can share one ring.

max_payload is a runtime parameter (default SHM_MAX_PAYLOAD, matching
UDP's MTU-driven MAX_PAYLOAD_BYTES for small messages), NOT a fixed
ceiling -- both sides just have to agree on the same value for a given
topic, same as they already have to agree on capacity. This is what
lets Image/CompressedImage frames go over local IPC as ONE slot,
whole, with no chunking: shared memory has no MTU, so the only reason
those types chunk over UDP (the 1400-byte payload cap) simply doesn't
apply here.

Layout: [ShmRingHeader(28 bytes)][slot 0][slot 1]...[slot capacity-1]
Each slot: uint32 seq_num, uint32 payload_len, uint8 payload[max_payload]

SPSC discipline: only the producer writes `tail`, only the consumer
writes `head`; each side only ever reads the other's index. Aligned
4-byte read/write is atomic at the hardware level on x86-64/ARM64, so
no OS-level lock is needed for the data path itself.
"""
import ctypes
import os
import struct
import time
from multiprocessing import shared_memory
from multiprocessing import resource_tracker
from typing import Optional, Tuple

SHM_RING_MAGIC = 0x524C4B31  # "RLK1"
SHM_RING_VERSION = 1
SHM_MAX_PAYLOAD = 1400  # default, matches MAX_PAYLOAD_BYTES (frame.py)
SHM_DEFAULT_CAPACITY = 1024
# Full 1920x1080 raw BGR8 frame -- default max_payload for
# advertise_local_ipc_image()/subscribe_local_ipc_image() so callers
# don't have to compute it themselves for the common case.
SHM_IMAGE_DEFAULT_MAX_PAYLOAD = 1920 * 1080 * 3


def _pid_alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True  # exists, just owned by someone else


class ShmRingHeader(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("magic", ctypes.c_uint32),
        ("version", ctypes.c_uint32),
        ("slot_size", ctypes.c_uint32),
        ("capacity", ctypes.c_uint32),
        ("head", ctypes.c_uint32),
        ("tail", ctypes.c_uint32),
        ("creator_pid", ctypes.c_uint32),
    ]


assert ctypes.sizeof(ShmRingHeader) == 28, "must match C++ ShmRingHeader exactly"


class ShmRing:
    """One ring per (topic, publisher-process, subscriber-process) pair
    -- strictly single-producer/single-consumer, same fan-out model as
    RelinkNode's existing per-peer UDP unicast."""

    def __init__(self):
        self._shm: Optional[shared_memory.SharedMemory] = None
        self._hdr: Optional[ShmRingHeader] = None
        self._capacity = 0
        self._creator = False
        self._name: Optional[str] = None
        self._max_payload = SHM_MAX_PAYLOAD
        self._slot_size = 8 + SHM_MAX_PAYLOAD

    def open(self, name: str, capacity: int = SHM_DEFAULT_CAPACITY, timeout: float = 2.0,
              max_payload: int = SHM_MAX_PAYLOAD) -> bool:
        posix_name = name[1:] if name.startswith("/") else name
        self._name = posix_name
        self._max_payload = max_payload
        self._slot_size = 8 + max_payload  # seq_num(4) + payload_len(4) + payload
        map_size = ctypes.sizeof(ShmRingHeader) + capacity * self._slot_size

        try:
            self._shm = shared_memory.SharedMemory(name=posix_name, create=True, size=map_size)
            self._creator = True
        except FileExistsError:
            self._shm = shared_memory.SharedMemory(name=posix_name, create=False)
            self._creator = False
            # CPython's resource_tracker registers EVERY SharedMemory
            # object on construction, creator or not (a known stdlib
            # wart, unfixed before 3.13's track= param -- see
            # bpo-38119/gh-82300). Left alone, this attacher's own
            # resource_tracker treats the segment as its responsibility
            # and unlinks it the moment THIS short-lived process exits
            # -- even though it never created it and a long-lived
            # creator/other attachers are still using it. That silently
            # detaches every subsequent attacher onto a brand-new blank
            # segment under the same name (open()'s create=True now
            # succeeds instead of hitting FileExistsError), so messages
            # published after the first attacher exits vanish with no
            # error anywhere. Only the creator may ever unlink (see
            # close()/unlink() and RelinkNode.request_stop() above) --
            # unregistering here just makes this process's tracker
            # honor that, instead of undermining it.
            try:
                resource_tracker.unregister(self._shm._name, "shared_memory")
            except Exception:
                pass

        self._hdr = ShmRingHeader.from_buffer(self._shm.buf)
        self._slots_offset = ctypes.sizeof(ShmRingHeader)
        self._capacity = capacity

        if self._creator:
            self._init_as_creator(capacity)
            return True

        # Attacher: the segment may be (a) mid-init by a live creator --
        # poll briefly, or (b) leftover from a creator that crashed
        # without unlinking -- detect via a dead creator_pid and steal
        # it rather than blindly attaching to whatever head/tail state a
        # crash left behind. NOTE: unlike the C++ side (a real CAS via
        # std::atomic::compare_exchange_strong), this is check-then-set,
        # not compare-and-swap -- Python has no portable stdlib atomic
        # RMW on shared memory. Two attachers detecting staleness at the
        # same instant both write identical version/slot_size/capacity/
        # head=0/tail=0, so the only race outcome is whose pid "wins"
        # creator_pid, not data corruption.
        deadline = time.monotonic() + timeout
        while True:
            if self._hdr.magic == SHM_RING_MAGIC:
                if self._hdr.version != SHM_RING_VERSION or self._hdr.slot_size != self._slot_size \
                        or self._hdr.capacity != capacity:
                    return False  # incompatible segment, not just stale
                owner = self._hdr.creator_pid
                if owner != os.getpid() and not _pid_alive(owner):
                    self._hdr.magic = 0  # best-effort steal, see note above
                    self._creator = True
                    self._init_as_creator(capacity)
                    return True
                return True  # owner alive (or is us) -- normal attach
            if time.monotonic() > deadline:
                return False
            time.sleep(0.001)

    def _init_as_creator(self, capacity: int):
        self._hdr.version = SHM_RING_VERSION
        self._hdr.slot_size = self._slot_size
        self._hdr.capacity = capacity
        self._hdr.head = 0
        self._hdr.tail = 0
        self._hdr.creator_pid = os.getpid()
        self._hdr.magic = SHM_RING_MAGIC  # publish last

    @property
    def is_creator(self) -> bool:
        return self._creator

    @property
    def max_payload(self) -> int:
        return self._max_payload

    def _slot_offset(self, index: int) -> int:
        return self._slots_offset + index * self._slot_size

    def try_push(self, payload: bytes, seq_num: int) -> bool:
        if len(payload) > self._max_payload:
            return False
        tail = self._hdr.tail
        head = self._hdr.head
        nxt = (tail + 1) % self._capacity
        if nxt == head:
            return False  # full -- explicit backpressure, not silent loss

        off = self._slot_offset(tail)
        buf = self._shm.buf
        struct.pack_into("<II", buf, off, seq_num, len(payload))
        buf[off + 8: off + 8 + len(payload)] = payload
        self._hdr.tail = nxt
        return True

    def try_pop(self) -> Optional[Tuple[int, bytes]]:
        """Returns (seq_num, payload_bytes) or None if empty. Always
        copies (an owned `bytes`), unlike the C++ side's
        try_pop_zero_copy() -- Python's raw UDP callbacks already always
        copy too (bytes slicing copies unconditionally; see frame.py's
        decode_frame()), so a zero-copy memoryview-into-shared-memory
        variant here would be a NEW sharp-edged lifetime contract with
        no existing precedent in this binding, for a language this
        project already documents as prioritizing convenience over
        matching the C++ core's performance (see udp_transport.py's
        module docstring). Not worth the risk for v1."""
        head = self._hdr.head
        tail = self._hdr.tail
        if head == tail:
            return None

        off = self._slot_offset(head)
        buf = self._shm.buf
        seq_num, payload_len = struct.unpack_from("<II", buf, off)
        payload = bytes(buf[off + 8: off + 8 + payload_len])
        self._hdr.head = (head + 1) % self._capacity
        return seq_num, payload

    def close(self):
        # _hdr (a ctypes structure built via from_buffer) holds a live
        # buffer export over self._shm.buf; SharedMemory.close() can't
        # release the mapping while that export is alive (raises
        # BufferError and leaks the resource_tracker's bookkeeping), so
        # it must be dropped first.
        self._hdr = None
        if self._shm is not None:
            self._shm.close()
            self._shm = None

    def unlink(self):
        self._hdr = None
        if self._shm is not None:
            self._shm.close()
            try:
                self._shm.unlink()
            except FileNotFoundError:
                pass
            self._shm = None

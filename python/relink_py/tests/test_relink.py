"""Plain-assert test suite for the Python binding, mirroring the style
and coverage of the C++ tests/test_*.cpp files -- no external test
framework dependency, prints ok/FAIL per check, exits nonzero on failure."""

import ctypes
import os
import subprocess
import sys
import threading
import time
from multiprocessing import shared_memory

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from relink.wire import RelinkHeader, Float32, Int32, is_wire_type
from relink.frame import encode_frame, decode_frame, FrameError, MAX_PAYLOAD_BYTES
from relink.register import (
    encode_register_request, decode_register_request,
    encode_register_ack, decode_register_ack, RegisterAckPeer,
)
from relink.beacon import encode_beacon_packet, decode_beacon_packet
from relink.udp_transport import UdpTransport, PeerAddr, ipv4_to_host_order
from relink.node import RelinkNode
from relink.shm_transport import ShmRing, SHM_RING_MAGIC

failures = 0


def check(cond, desc):
    global failures
    if cond:
        print(f"ok: {desc}")
    else:
        print(f"FAIL: {desc}", file=sys.stderr)
        failures += 1


def check_throws(fn, desc):
    global failures
    try:
        fn()
        print(f"FAIL: expected exception: {desc}", file=sys.stderr)
        failures += 1
    except (RuntimeError, TypeError):
        print(f"ok: throws: {desc}")


# --- wire.py ---
check(ctypes.sizeof(RelinkHeader) == 9, "RelinkHeader is 9 bytes")
check(ctypes.sizeof(Float32) == 4, "Float32 is 4 bytes")
check(is_wire_type(Int32), "Int32 is a valid wire type")


class NotPacked(ctypes.Structure):
    _fields_ = [("x", ctypes.c_int)]


check(not is_wire_type(NotPacked), "un-packed struct rejected as wire type")

# --- frame.py round trip ---
class Msg(ctypes.Structure):
    _pack_ = 1
    _fields_ = [("a", ctypes.c_float), ("b", ctypes.c_uint64)]


msg = Msg(a=3.14, b=123456789)
frame = encode_frame(42, 7, bytes(msg))
decoded = decode_frame(frame)
check(decoded.topic_id == 42, "frame round trip: topic_id")
check(decoded.seq_num == 7, "frame round trip: seq_num")
out = Msg.from_buffer_copy(decoded.payload)
check(abs(out.a - 3.14) < 1e-5, "frame round trip: payload float field")
check(out.b == 123456789, "frame round trip: payload uint64 field")

# payload containing raw 0x0A/0x23 must decode correctly (never-scan rule)
tricky_payload = bytes([0x0A, 0x23, 0x0A, 0x00, 0x23])
tricky_frame = encode_frame(9, 1, tricky_payload)
tricky_decoded = decode_frame(tricky_frame)
check(tricky_decoded.payload == tricky_payload, "frame survives embedded 0x0A/0x23 bytes")

try:
    encode_frame(1, 0, bytes(MAX_PAYLOAD_BYTES + 1))
    check(False, "oversized payload should raise FrameError")
except FrameError:
    check(True, "oversized payload rejected at encode")

# --- register.py round trip ---
req = encode_register_request(0x0A000005, 5555, [100, 101, 200])
dreq = decode_register_request(req)
check(dreq.node_ip == 0x0A000005 and dreq.topic_ids == [100, 101, 200], "RegisterRequest round trip")

ack = encode_register_ack(0, [
    RegisterAckPeer(0x0A000006, 6000, 100, 0xC0A80006, 6000),  # has a LAN candidate
    RegisterAckPeer(0x0A000007, 7000, 101),                    # no LAN candidate known (defaults to 0, 0)
])
dack = decode_register_ack(ack)
check(dack.status == 0 and dack.peers[0].port == 6000, "RegisterAck round trip")
check(dack.peers[0].lan_ip == 0xC0A80006 and dack.peers[0].lan_port == 6000, "RegisterAckPeer LAN candidate round trip")
check(dack.peers[1].lan_ip == 0 and dack.peers[1].lan_port == 0, "RegisterAckPeer with no LAN candidate round trip")

# --- beacon.py round trip ---
beacon = encode_beacon_packet(0x0A000005, 5000, [100, 200])
dbeacon = decode_beacon_packet(beacon)
check(dbeacon.node_port == 5000 and dbeacon.topic_ids == [100, 200], "BeaconPacket round trip")

# --- ShmRing (local IPC) round trip, backpressure, and crash recovery ---
_shm_test_name = "/relink_test_shm_ring_py"
try:
    shared_memory.SharedMemory(name=_shm_test_name[1:]).unlink()
except FileNotFoundError:
    pass

ring = ShmRing()
check(ring.open(_shm_test_name, capacity=4), "ShmRing.open() as creator")
check(ring.is_creator, "ShmRing.open() became creator (first opener)")
check(ring.try_push(b"12345678", 42), "ShmRing.try_push() basic")
popped = ring.try_pop()
check(popped is not None and popped == (42, b"12345678"), "ShmRing.try_pop() round trip")
check(ring.try_pop() is None, "ShmRing.try_pop() empty after drain")

pushed_count = sum(1 for i in range(10) if ring.try_push(b"xxxx", i))
check(pushed_count == 3, "ShmRing full-ring backpressure: exactly capacity-1, not silently more/fewer")
popped_count = 0
while ring.try_pop() is not None:
    popped_count += 1
check(popped_count == 3, "every accepted push is retrievable, none lost")

ring.unlink()  # release _shm_test_name before reusing a fresh name below

ring2 = ShmRing()
check(ring2.open("/relink_test_shm_ring_py2", capacity=4, max_payload=8), "open with explicit max_payload")
check(not ring2.try_push(b"123456789", 0), "oversized push (9 > max_payload=8) rejected")
check(ring2.try_push(b"12345678", 0), "push at exactly max_payload succeeds")
ring2.unlink()

# Stale-segment takeover: a creator that crashes leaves a segment
# behind with a valid header but a dead creator_pid; the next opener
# must detect that and reinitialize rather than attach to garbage
# head/tail state. Synthesizes that condition directly (a valid header,
# a CONFIRMED-dead pid, non-zero tail) instead of relying on real
# process-crash timing: os.fork() is fragile here (CPython's
# multiprocessing.shared_memory keeps a background resource_tracker
# connection that doesn't survive fork cleanly), and subprocess+kill()
# races against that SAME resource_tracker's own automatic crash
# cleanup (confirmed while writing this test -- Python auto-reaps a
# killed process's shared memory asynchronously, unlike raw POSIX shm
# in C++, so "leave it and check it's still there" isn't deterministic
# timing in Python). A confirmed-dead, real pid (an unrelated process
# that already fully exited) sidesteps both problems.
from relink.shm_transport import ShmRingHeader, SHM_RING_VERSION
import subprocess as _subprocess

_dead_child = _subprocess.Popen([sys.executable, "-c", "pass"])
_dead_child.wait()  # now a definite, real, already-exited pid
_dead_pid = _dead_child.pid

_shm_crash_name = "/relink_test_shm_crash_py"
try:
    shared_memory.SharedMemory(name=_shm_crash_name[1:]).unlink()
except FileNotFoundError:
    pass

_stale_shm = shared_memory.SharedMemory(name=_shm_crash_name[1:], create=True,
                                         size=ctypes.sizeof(ShmRingHeader) + 4 * (8 + 1400))
_stale_hdr = ShmRingHeader.from_buffer(_stale_shm.buf)
_stale_hdr.version = SHM_RING_VERSION
_stale_hdr.slot_size = 8 + 1400
_stale_hdr.capacity = 4
_stale_hdr.head = 0
_stale_hdr.tail = 1  # simulate "something was pushed and never consumed"
_stale_hdr.creator_pid = _dead_pid
_stale_hdr.magic = SHM_RING_MAGIC  # publish last, marks it as a live-looking (but stale) segment
del _stale_hdr  # drop the buffer export before close(), same BufferError hazard as ShmRing.close()
_stale_shm.close()  # NOT unlink() -- leaves it behind, exactly like a real crash would

ring3 = ShmRing()
check(ring3.open(_shm_crash_name, capacity=4), "fresh open() after synthesized stale (dead-pid) segment")
check(ring3.is_creator, "detected dead creator_pid and stole creator role")
check(ring3.try_pop() is None, "reset to empty, not inheriting the stale tail=1 state")
ring3.unlink()

# --- RelinkNode-level local IPC (advertise/subscribe/publish_local_ipc) ---
ipc_node = RelinkNode()
ipc_received = []
check(ipc_node.subscribe_local_ipc(950, lambda p: ipc_received.append(p)),
      "RelinkNode.subscribe_local_ipc() succeeds")
check(ipc_node.publish_local_ipc(950, b"node-level-ipc"), "RelinkNode.publish_local_ipc() succeeds")
_deadline = time.time() + 2.0
while time.time() < _deadline and not ipc_received:
    time.sleep(0.01)
check(ipc_received == [b"node-level-ipc"], "local IPC message delivered through RelinkNode's poll thread")
ipc_node.request_stop()

# --- UdpTransport real socket loopback test ---
recv_tp = UdpTransport()
recv_tp.bind(0)
received = []
lock = threading.Lock()


def on_msg(payload: bytes):
    with lock:
        received.append(payload)


recv_tp.set_topic_handler(50, on_msg)
recv_tp.start()

send_tp = UdpTransport()
send_tp.bind(0)
send_tp.start()

loopback = ipv4_to_host_order("127.0.0.1")
dest = PeerAddr(loopback, recv_tp.local_port)
ok = send_tp.publish_raw(50, b"hello-relink", dest)
check(ok, "publish_raw over real loopback socket returns True")

deadline = time.time() + 2.0
while time.time() < deadline and not received:
    time.sleep(0.02)
with lock:
    check(len(received) == 1 and received[0] == b"hello-relink", "real UDP delivery over loopback")

recv_tp.stop()
send_tp.stop()

# --- RelinkNode config error paths ---
node = RelinkNode()
node.set_rlcore.ip("127.0.0.1")
check_throws(lambda: node.use_multicast_discovery(), "mutual exclusivity: rlcore then multicast")

node2 = RelinkNode()
node2.use_multicast_discovery()
check_throws(lambda: node2.set_rlcore.ip("127.0.0.1"), "mutual exclusivity: multicast then rlcore")

node3 = RelinkNode()
check_throws(lambda: node3.spin_once(), "neither configured -> throws at spin time")

node4 = RelinkNode()
node4.set_rlcore.port(9000)
node4.advertise(100, Int32)
check_throws(lambda: node4.publish(100, Int32(data=42)), "port without ip -> throws")

# --- rl_topic CLI: --ipc guard on list/info ---
# Mirrors cpp/tests/test_rl_topic_cli.cpp. list/info have no --ipc
# argument at all (see build_parser()) -- there is no central directory
# of same-host-only IPC topics for them to query, unlike UDP topics
# which rlcore/multicast can always answer about. argparse itself must
# reject --ipc there with a nonzero exit, not hang or silently ignore it.
_RELINK_PY_DIR = os.path.join(os.path.dirname(__file__), "..")


def _run_rl_topic(args):
    return subprocess.run(
        [sys.executable, "-m", "relink.cli.rl_topic"] + args,
        cwd=_RELINK_PY_DIR, capture_output=True, text=True, timeout=10,
    )


_r = _run_rl_topic(["list", "--ipc"])
check(_r.returncode == 2, "rltopic list --ipc: exits nonzero (argparse rejects unknown arg)")
check("--ipc" in _r.stderr, "rltopic list --ipc: stderr names the rejected argument")

_r = _run_rl_topic(["info", "/some/topic", "--ipc"])
check(_r.returncode == 2, "rltopic info --ipc: exits nonzero (argparse rejects unknown arg)")
check("--ipc" in _r.stderr, "rltopic info --ipc: stderr names the rejected argument")

# Regression guard the other direction: pub --ipc (which DOES accept
# --ipc, and returns immediately -- no peer-wait loop, unlike UDP) must
# not be broken by anything guarding list/info.
_r = _run_rl_topic(["pub", "/relink/test_cli_ipc_guard", "--ipc", "--text", "hello"])
check(_r.returncode == 0, "rltopic pub --ipc: still exits 0")
check("published" in _r.stdout, "rltopic pub --ipc: still publishes")

print()
if failures == 0:
    print("ALL PASS")
    sys.exit(0)
else:
    print(f"{failures} FAILURE(S)")
    sys.exit(1)

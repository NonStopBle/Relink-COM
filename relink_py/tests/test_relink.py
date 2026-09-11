"""Plain-assert test suite for the Python binding, mirroring the style
and coverage of the C++ tests/test_*.cpp files -- no external test
framework dependency, prints ok/FAIL per check, exits nonzero on failure."""

import ctypes
import os
import sys
import threading
import time

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
check(ctypes.sizeof(RelinkHeader) == 7, "RelinkHeader is 7 bytes")
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

ack = encode_register_ack(0, [RegisterAckPeer(0x0A000006, 6000, 100)])
dack = decode_register_ack(ack)
check(dack.status == 0 and dack.peers[0].port == 6000, "RegisterAck round trip")

# --- beacon.py round trip ---
beacon = encode_beacon_packet(0x0A000005, 5000, [100, 200])
dbeacon = decode_beacon_packet(beacon)
check(dbeacon.node_port == 5000 and dbeacon.topic_ids == [100, 200], "BeaconPacket round trip")

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
node.set_com_core.ip("127.0.0.1")
check_throws(lambda: node.use_multicast_discovery(), "mutual exclusivity: comcore then multicast")

node2 = RelinkNode()
node2.use_multicast_discovery()
check_throws(lambda: node2.set_com_core.ip("127.0.0.1"), "mutual exclusivity: multicast then comcore")

node3 = RelinkNode()
check_throws(lambda: node3.spin_once(), "neither configured -> throws at spin time")

node4 = RelinkNode()
node4.set_com_core.port(9000)
node4.advertise(100, Int32)
check_throws(lambda: node4.publish(100, Int32(data=42)), "port without ip -> throws")

print()
if failures == 0:
    print("ALL PASS")
    sys.exit(0)
else:
    print(f"{failures} FAILURE(S)")
    sys.exit(1)

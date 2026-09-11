# ReLink -- Python binding

Independent Python re-implementation of the wire protocol documented in
`relink-com-spec.md`, stdlib only (`ctypes` + `socket` + `struct`), not a
wrapper around the C++ library (`relink/`). Proven wire-compatible with
the C++ node in both directions and over both discovery modes -- see
`tests/two_process_pub.py` / `tests/two_process_sub.py`.

## Install

No build step -- it's pure Python stdlib:

```
cd relink_py
python3 -c "import relink"   # or add relink_py/ to PYTHONPATH
```

## Quick start

```python
import ctypes
from relink import RelinkNode, Float32

class ImuReading(ctypes.Structure):
    _pack_ = 1
    _fields_ = [("accel_x", ctypes.c_float), ("accel_y", ctypes.c_float), ("accel_z", ctypes.c_float)]

node = RelinkNode()
node.set_com_core.ip("10.0.0.5")          # mode A, or node.use_multicast_discovery() for mode B

node.advertise(100, ImuReading)
node.publish(100, ImuReading(accel_x=0.1, accel_y=0.2, accel_z=9.81))

node.subscribe(101, Float32, lambda msg: print(msg.data))
node.spin()
```

Custom types are plain `ctypes.Structure` subclasses with `_pack_ = 1` --
the Python analog of C++'s `#pragma pack(1)` + `std::is_trivially_copyable`
contract. No registration, no schema exchange: any conforming struct just
works.

## Examples

- `examples/comcore_pubsub.py` -- mode A, mirrors `relink_example.cpp`
- `examples/multicast_pubsub.py` -- mode B

Run as two processes: `python3 comcore_pubsub.py pub <com_core_ip>` and
`python3 comcore_pubsub.py sub <com_core_ip>`.

## Tests

```
cd relink_py
python3 tests/test_relink.py
```

Plain-assert suite mirroring the C++ `tests/test_*.cpp` coverage: wire
layout, frame codec, register/beacon codec, real-socket UDP delivery,
and RelinkNode's discovery-mode error paths.

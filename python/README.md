# ReLink -- Python package

This directory holds the pip-installable Python distribution of ReLink:
the `relink` library (a from-scratch, stdlib-only reimplementation of
the wire protocol -- see [`relink_py/README.md`](relink_py/README.md)
for the full technical background) and two command-line tools that
install onto your `PATH` alongside it.

## Install

```bash
cd python/relink_py
pip install -e .          # editable install; drop -e for a normal install
```

No third-party dependencies -- the whole package is Python stdlib
(`socket`, `struct`, `ctypes`). This registers the `relink` package
and two console scripts on your `PATH`:

| Command | What it runs | Purpose |
|---|---|---|
| `rlcore` | `relink.cli.rlcore:main` | The Mode A registration daemon, plus (`--relay`/`--nat`) the NAT-fallback relay -- see below |
| `rltopic` | `relink.cli.rl_topic:main` | `rostopic`-style topic inspection CLI |

Verify the install:

```bash
which rlcore rltopic
```

## Using the commands

```bash
rlcore --port 8445                     # start the registration daemon
rlcore --nat                           # with NAT traversal / hole punching (implies --relay)
rlcore --relay                         # relay-fallback forwarding, on this same port
rlcore --generate-key                  # print a fresh AES-256 key, then exit
rlcore --encrypt-key <64-hex>          # require encrypted registration

rltopic list --rlcore-ip 203.0.113.10
rltopic info /example/chatter --rlcore-ip 203.0.113.10
rltopic echo /example/chatter --rlcore-ip 203.0.113.10
rltopic list                           # no --rlcore-ip: multicast discovery instead

rltopic echo /my/ipc/topic --ipc       # same-host shared-memory IPC instead of the
rltopic pub /my/ipc/topic --ipc --text "hi"  # network -- no rlcore/multicast involved at all
```

`--ipc` works on `hz`/`bw`/`echo`/`pub` (not `list`/`info`, which have
nothing to query for a same-host-only topic) and resolves `<topic>` to
a numeric id with the same name hash the network path uses, then talks
directly to the shared-memory ring (`advertise_local_ipc`/
`subscribe_local_ipc`/`publish_local_ipc`) -- see [Same-host
IPC](relink_py/README.md#same-host-ipc) below.

Unlike the C++ build, which keeps `relink-relay` as its own separate
binary/process, the Python build folds relay-fallback forwarding
directly into `rlcore` -- one process, one port, enabled with `--relay`
(or automatically via `--nat`, since that's exactly the case where a
client might have a NAT type direct punching can't cross). On the
client, point `node.set_relay(ip, port)` at this same `rlcore` and
`--port` to use it (not the old standalone relay's default of 8446).

These are pure-Python equivalents of the C++ `relink-rlcore` and
`rl_topic` binaries under [`../cpp/`](../cpp/README.md) -- same wire
protocol, same registration/discovery flags, so a Python `rlcore` and a
C++ node (or vice versa) interoperate with no changes on either side.
(`relink-relay` itself stays a separate binary only in the C++ build.)

## Using the library

Once installed, `import relink` works from anywhere, not just this
directory:

```python
from relink import RelinkNode, Float32

node = RelinkNode()
node.use_multicast_discovery()
node.subscribe(101, Float32, lambda msg: print(msg.data))
node.spin()
```

See [`relink_py/README.md`](relink_py/README.md) for the full API,
message types, and the "why this exists" technical writeup, and
[`relink_py/examples/`](relink_py/examples/) for runnable examples.

## Package layout

```
python/
  README.md              this file
  relink_py/              the installable distribution (pyproject.toml lives here)
    pyproject.toml          package metadata + the two console_scripts entries
    relink/                  the `relink` package
      cli/                     the command-line tools (installed as scripts above)
        rlcore.py                registration daemon + relay fallback (--relay/--nat)
        rl_topic.py               topic inspection CLI
      node.py, wire.py, ...    the library itself (see relink_py/README.md)
    examples/                runnable example nodes
    tests/                   plain-assert test suite (`python3 tests/test_relink.py`)
```

## Uninstall

```bash
pip uninstall relink
```

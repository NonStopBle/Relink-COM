# ReLink -- Python package

This directory holds the pip-installable Python distribution of ReLink:
the `relink` library (a from-scratch, stdlib-only reimplementation of
the wire protocol -- see [`relink_py/README.md`](relink_py/README.md)
for the full technical background) and three command-line tools that
install onto your `PATH` alongside it.

## Install

```bash
cd python/relink_py
pip install -e .          # editable install; drop -e for a normal install
```

No third-party dependencies -- the whole package is Python stdlib
(`socket`, `struct`, `ctypes`). This registers the `relink` package
and three console scripts on your `PATH`:

| Command | What it runs | Purpose |
|---|---|---|
| `rlcore` | `relink.cli.rlcore:main` | The Mode A registration daemon (Python build) |
| `rltopic` | `relink.cli.rl_topic:main` | `rostopic`-style topic inspection CLI |
| `relink-relay` | `relink.cli.relay:main` | NAT-fallback UDP relay daemon (Python build) |

Verify the install:

```bash
which rlcore rltopic relink-relay
```

## Using the commands

```bash
rlcore --port 8445                     # start the registration daemon
rlcore --nat                           # with NAT traversal / hole punching
rlcore --generate-key                  # print a fresh AES-256 key, then exit
rlcore --encrypt-key <64-hex>          # require encrypted registration

rltopic list --rlcore-ip 203.0.113.10
rltopic info /example/chatter --rlcore-ip 203.0.113.10
rltopic echo /example/chatter --rlcore-ip 203.0.113.10
rltopic list                           # no --rlcore-ip: multicast discovery instead

relink-relay 8446                      # NAT-fallback relay, listening on port 8446
```

These are pure-Python equivalents of the C++ `relink-rlcore`,
`rl_topic`, and `relink-relay` binaries under
[`../cpp/`](../cpp/README.md) -- same wire protocol, same flags, so a
Python `rlcore` and a C++ node (or vice versa) interoperate with no
changes on either side.

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
    pyproject.toml          package metadata + the three console_scripts entries
    relink/                  the `relink` package
      cli/                     the three command-line tools (installed as scripts above)
        rlcore.py                registration daemon
        rl_topic.py               topic inspection CLI
        relay.py                  NAT-fallback relay daemon
      node.py, wire.py, ...    the library itself (see relink_py/README.md)
    examples/                runnable example nodes
    tests/                   plain-assert test suite (`python3 tests/test_relink.py`)
```

## Uninstall

```bash
pip uninstall relink
```

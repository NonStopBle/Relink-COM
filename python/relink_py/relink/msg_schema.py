"""Custom message schema files for rl_topic.py's `echo` -- lets a user
describe their own wire-format message in a small ROS-.msg-style text
file and hand rl_topic its path (same idea as `--rlcore-ip`: a path/
value passed on the command line, not something baked into the tool),
so `echo` can decode and pretty-print field values instead of raw hex.

Grammar, one field per line, ordered (order = wire layout, same
convention as every ctypes.Structure in this codebase):
    <type> <name>            scalar field
    <type>[N] <name>         fixed-size array of N elements
    string[N] <name>         fixed N-byte char buffer (NOT
                              null-terminated unless N is generous --
                              same fixed-size-only departure from real
                              ROS msgs as standard_msgs.py/.hpp)
    # comment                ignored, as is a blank line

<type> is one of: bool, int8, uint8, int16, uint16, int32, uint32,
int64, uint64, float32, float64, string.

No nested custom types, no variable-length fields -- matches every
built-in message type in this codebase (see standard_msgs.py's module
docstring for why: fixed-size only is what makes a ctypes.Structure a
valid ReLink wire type in the first place, see wire.py's is_wire_type()).

Example (imu.msg):
    float32 accel_x
    float32 accel_y
    float32 accel_z
    uint64 timestamp_us
    string[16] sensor_name
"""
import ctypes
import re

_TYPE_MAP = {
    "bool": ctypes.c_bool,
    "int8": ctypes.c_int8, "uint8": ctypes.c_uint8,
    "int16": ctypes.c_int16, "uint16": ctypes.c_uint16,
    "int32": ctypes.c_int32, "uint32": ctypes.c_uint32,
    "int64": ctypes.c_int64, "uint64": ctypes.c_uint64,
    "float32": ctypes.c_float, "float64": ctypes.c_double,
}

_LINE_RE = re.compile(
    r'^(?P<type>[a-zA-Z0-9_]+)(\[(?P<arraylen>\d+)\])?\s+(?P<name>[a-zA-Z_][a-zA-Z0-9_]*)\s*$'
)


class MsgSchemaError(ValueError):
    pass


def parse_msg_file(path: str):
    """Parses a .msg schema file (see module docstring) into a fresh
    ctypes.LittleEndianStructure subclass (_pack_ = 1, matching every
    other wire type in this codebase). Raises MsgSchemaError with a
    line number on any malformed line or unknown type -- never returns
    a partially-built type."""
    fields = []
    try:
        with open(path) as f:
            lines = f.readlines()
    except OSError as e:
        raise MsgSchemaError(f"cannot read {path}: {e}")

    for lineno, raw_line in enumerate(lines, 1):
        line = raw_line.split("#", 1)[0].strip()
        if not line:
            continue
        m = _LINE_RE.match(line)
        if not m:
            raise MsgSchemaError(f"{path}:{lineno}: cannot parse {raw_line.rstrip()!r}")
        type_name = m.group("type")
        array_len = m.group("arraylen")
        name = m.group("name")
        if type_name == "string":
            n = int(array_len) if array_len else 64
            ctype = ctypes.c_char * n
        elif type_name in _TYPE_MAP:
            base = _TYPE_MAP[type_name]
            ctype = (base * int(array_len)) if array_len else base
        else:
            raise MsgSchemaError(f"{path}:{lineno}: unknown type {type_name!r} "
                                  f"(expected one of: {', '.join(sorted(_TYPE_MAP))}, string)")
        fields.append((name, ctype))

    if not fields:
        raise MsgSchemaError(f"{path}: no fields found")

    return type("CustomMsg", (ctypes.LittleEndianStructure,), {
        "_pack_": 1,
        "_fields_": fields,
    })


def format_message(instance) -> str:
    """Pretty-prints a ctypes.Structure instance's fields, one per
    line -- used once a payload has been successfully decoded against a
    known (--type) or custom (--msg) schema, instead of raw hex."""
    lines = []
    for name, _ in instance._fields_:
        value = getattr(instance, name)
        if isinstance(value, bytes):
            value = value.rstrip(b"\x00").decode("utf-8", errors="replace")
        elif hasattr(value, "__len__") and not isinstance(value, (str, bytes)):
            value = list(value)
        lines.append(f"  {name} = {value}")
    return "\n".join(lines)


def find_builtin_type(name: str):
    """Looks up `name` (e.g. "Float32", "Imu", "Pose") across every
    built-in message module rl_topic.py knows about, for --type. Returns
    None if not found or if it's not actually a wire-format ctypes
    struct (guards against matching an unrelated same-named symbol like
    a helper function)."""
    import relink
    from relink import std_msgs, geometry_msgs, sensor_msgs, nav_msgs
    from relink import diagnostic_msgs, trajectory_msgs, actionlib_msgs
    from relink.wire import is_wire_type

    for module in (relink, std_msgs, geometry_msgs, sensor_msgs, nav_msgs,
                   diagnostic_msgs, trajectory_msgs, actionlib_msgs):
        candidate = getattr(module, name, None)
        if candidate is not None and isinstance(candidate, type) and is_wire_type(candidate):
            return candidate
    return None

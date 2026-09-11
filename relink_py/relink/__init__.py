"""ReLink -- Python binding.

Independent re-implementation of the wire protocol documented in
relink-com-spec.md, stdlib-only (ctypes + socket + struct), not a wrapper
around the C++ library, per the spec's "any language, any socket" rule.
"""

from .wire import (
    Bool, Byte, Char, Int8, Int16, Int32, Int64,
    UInt8, UInt16, UInt32, UInt64, Float32, Float64,
    is_wire_type,
)
from .node import RelinkNode, DiscoveryMode
from .register import COM_CORE_DEFAULT_PORT
from .multicast_discovery import DEFAULT_MULTICAST_GROUP, DEFAULT_MULTICAST_PORT

__all__ = [
    "RelinkNode", "DiscoveryMode",
    "Bool", "Byte", "Char", "Int8", "Int16", "Int32", "Int64",
    "UInt8", "UInt16", "UInt32", "UInt64", "Float32", "Float64",
    "is_wire_type",
    "COM_CORE_DEFAULT_PORT", "DEFAULT_MULTICAST_GROUP", "DEFAULT_MULTICAST_PORT",
]

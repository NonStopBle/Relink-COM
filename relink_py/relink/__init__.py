"""ReLink -- Python binding.

Independent re-implementation of ReLink's wire protocol, stdlib-only
(ctypes + socket + struct), not a wrapper around the C++ library --
"any language, any socket" can speak it.
"""

from .wire import (
    Bool, Byte, Char, Int8, Int16, Int32, Int64,
    UInt8, UInt16, UInt32, UInt64, Float32, Float64,
    is_wire_type,
)
from .node import RelinkNode, DiscoveryMode
from .register import COM_CORE_DEFAULT_PORT
from .multicast_discovery import DEFAULT_MULTICAST_GROUP, DEFAULT_MULTICAST_PORT
from .image import ImageChunk, ImageReassembler, ImageTooLargeError, MAX_IMAGE_BYTES

__all__ = [
    "RelinkNode", "DiscoveryMode",
    "Bool", "Byte", "Char", "Int8", "Int16", "Int32", "Int64",
    "UInt8", "UInt16", "UInt32", "UInt64", "Float32", "Float64",
    "is_wire_type",
    "COM_CORE_DEFAULT_PORT", "DEFAULT_MULTICAST_GROUP", "DEFAULT_MULTICAST_PORT",
    "ImageChunk", "ImageReassembler", "ImageTooLargeError", "MAX_IMAGE_BYTES",
]

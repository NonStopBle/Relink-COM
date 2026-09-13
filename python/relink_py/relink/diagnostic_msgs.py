"""ROS-familiar import path: `from relink import diagnostic_msgs; diagnostic_msgs.DiagnosticStatus`.
Real definitions live in standard_msgs.py."""
from .standard_msgs import (
    KeyValue, DiagnosticStatus, DiagnosticArray,
    KV_MAX_LEN, MAX_KEY_VALUES, MAX_DIAGNOSTIC_STATUSES,
)

__all__ = ["KeyValue", "DiagnosticStatus", "DiagnosticArray",
           "KV_MAX_LEN", "MAX_KEY_VALUES", "MAX_DIAGNOSTIC_STATUSES"]

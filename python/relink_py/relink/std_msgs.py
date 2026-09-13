"""ROS-familiar import path: `from relink import std_msgs; std_msgs.Header`.
Real definitions live in standard_msgs.py (shared with geometry_msgs.py/
sensor_msgs.py/nav_msgs.py) -- this just re-exports under the expected name."""
from .standard_msgs import Empty, Time, Duration, ColorRGBA, Header, String, FRAME_ID_MAX_LEN, STRING_MAX_LEN

__all__ = ["Empty", "Time", "Duration", "ColorRGBA", "Header", "String",
           "FRAME_ID_MAX_LEN", "STRING_MAX_LEN"]

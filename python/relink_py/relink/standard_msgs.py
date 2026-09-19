"""Standard message types -- Python mirror of standard_msgs.hpp. See
that file for the full rationale, including the three deliberate
departures from ROS's actual wire format:
  1. No variable-length strings -- fixed-size char buffers instead.
  2. float32 (not ROS's float64) for geometry_msgs, except NavSatFix's
     lat/lon/altitude.
  3. No variable-length arrays -- fixed-capacity array + count field.
Every cap constant here (kMax...) must match its C++ counterpart
exactly -- that's what proven cross-language wire-compatibility means
(see tests/test_standard_msgs.py, the cross-language checks).
Every type is also checked against MAX_PAYLOAD_BYTES (1400, frame.py)
at import time, the same guarantee standard_msgs.hpp's static_asserts
give the C++ side -- an oversized type fails loudly here, not silently
at publish() time.
"""
import ctypes
import time

MAX_PAYLOAD_BYTES = 1400  # relink/include/relink/frame.hpp's kMaxPayloadBytes


def _check_fits(cls):
    size = ctypes.sizeof(cls)
    if size > MAX_PAYLOAD_BYTES:
        raise TypeError(f"{cls.__name__} is {size} bytes, exceeds MAX_PAYLOAD_BYTES "
                         f"({MAX_PAYLOAD_BYTES}) -- shrink one of its kMax* caps")
    return cls


# --- std_msgs -----------------------------------------------------------

FRAME_ID_MAX_LEN = 32
STRING_MAX_LEN = 64


class Empty(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [("_unused", ctypes.c_uint8)]  # ctypes has no true zero-size struct


class Time(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [("sec", ctypes.c_uint32), ("nsec", ctypes.c_uint32)]

    @staticmethod
    def now() -> "Time":
        t = time.time()
        return Time(sec=int(t), nsec=int((t - int(t)) * 1e9))


class Duration(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [("sec", ctypes.c_int32), ("nsec", ctypes.c_int32)]


class ColorRGBA(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [("r", ctypes.c_float), ("g", ctypes.c_float),
                ("b", ctypes.c_float), ("a", ctypes.c_float)]


class Header(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [("seq", ctypes.c_uint32), ("stamp", Time),
                ("_frame_id", ctypes.c_char * FRAME_ID_MAX_LEN)]

    def set_frame_id(self, name: str):
        self._frame_id = name.encode("utf-8")[:FRAME_ID_MAX_LEN - 1]

    def frame_id_str(self) -> str:
        return self._frame_id.rstrip(b"\x00").decode("utf-8", errors="replace")

    def stamp_now(self):
        self.stamp = Time.now()


class String(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [("_data", ctypes.c_char * STRING_MAX_LEN)]

    def set(self, s: str):
        self._data = s.encode("utf-8")[:STRING_MAX_LEN - 1]

    def str(self) -> str:
        return self._data.rstrip(b"\x00").decode("utf-8", errors="replace")


for _t in (Empty, Time, Duration, ColorRGBA, Header, String):
    _check_fits(_t)


# --- geometry_msgs --------------------------------------------------------

MAX_POSES_IN_ARRAY = 16
MAX_POLYGON_POINTS = 16


class Vector3(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [("x", ctypes.c_float), ("y", ctypes.c_float), ("z", ctypes.c_float)]


class Point(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [("x", ctypes.c_float), ("y", ctypes.c_float), ("z", ctypes.c_float)]


class Point32(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [("x", ctypes.c_float), ("y", ctypes.c_float), ("z", ctypes.c_float)]


class Quaternion(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [("x", ctypes.c_float), ("y", ctypes.c_float),
                ("z", ctypes.c_float), ("w", ctypes.c_float)]


class Pose(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [("position", Point), ("orientation", Quaternion)]


class Twist(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [("linear", Vector3), ("angular", Vector3)]


class Accel(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [("linear", Vector3), ("angular", Vector3)]


class Wrench(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [("force", Vector3), ("torque", Vector3)]


class PoseStamped(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [("header", Header), ("pose", Pose)]


class TwistStamped(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [("header", Header), ("twist", Twist)]


class Transform(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [("translation", Vector3), ("rotation", Quaternion)]


class TransformStamped(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [("header", Header), ("_child_frame_id", ctypes.c_char * FRAME_ID_MAX_LEN),
                ("transform", Transform)]

    def set_child_frame_id(self, name: str):
        self._child_frame_id = name.encode("utf-8")[:FRAME_ID_MAX_LEN - 1]

    def child_frame_id_str(self) -> str:
        return self._child_frame_id.rstrip(b"\x00").decode("utf-8", errors="replace")


class PoseWithCovariance(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [("pose", Pose), ("covariance", ctypes.c_float * 36)]


class TwistWithCovariance(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [("twist", Twist), ("covariance", ctypes.c_float * 36)]


class PoseArray(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [("header", Header), ("count", ctypes.c_uint32), ("poses", Pose * MAX_POSES_IN_ARRAY)]


class Polygon(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [("count", ctypes.c_uint32), ("points", Point32 * MAX_POLYGON_POINTS)]


for _t in (Vector3, Point, Point32, Quaternion, Pose, Twist, Accel, Wrench, PoseStamped,
           TwistStamped, Transform, TransformStamped, PoseWithCovariance, TwistWithCovariance,
           PoseArray, Polygon):
    _check_fits(_t)


# --- sensor_msgs ----------------------------------------------------------

MAX_DISTORTION_COEFFS = 8
MAX_POINT_FIELDS = 4
MAX_POINT_CLOUD_BYTES = 900
MAX_LASER_SCAN_POINTS = 120
MAX_JOINTS = 8


class Imu(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("header", Header),
        ("orientation", Quaternion),
        ("orientation_covariance", ctypes.c_float * 9),
        ("angular_velocity", Vector3),
        ("angular_velocity_covariance", ctypes.c_float * 9),
        ("linear_acceleration", Vector3),
        ("linear_acceleration_covariance", ctypes.c_float * 9),
    ]


class NavSatStatus(ctypes.LittleEndianStructure):
    _pack_ = 1
    NO_FIX, FIX, SBAS_FIX, GBAS_FIX = -1, 0, 1, 2
    SERVICE_GPS, SERVICE_GLONASS, SERVICE_COMPASS, SERVICE_GALILEO = 1, 2, 4, 8
    _fields_ = [("status", ctypes.c_int8), ("service", ctypes.c_uint16)]


class NavSatFix(ctypes.LittleEndianStructure):
    _pack_ = 1
    COVARIANCE_UNKNOWN, COVARIANCE_APPROXIMATED, COVARIANCE_DIAGONAL_KNOWN, COVARIANCE_KNOWN = 0, 1, 2, 3
    _fields_ = [
        ("header", Header),
        ("status", NavSatStatus),
        ("latitude", ctypes.c_double),
        ("longitude", ctypes.c_double),
        ("altitude", ctypes.c_double),
        ("position_covariance", ctypes.c_double * 9),
        ("position_covariance_type", ctypes.c_uint8),
    ]


class MagneticField(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [("header", Header), ("magnetic_field", Vector3),
                ("magnetic_field_covariance", ctypes.c_float * 9)]


class Temperature(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [("header", Header), ("temperature", ctypes.c_double), ("variance", ctypes.c_double)]


class Range(ctypes.LittleEndianStructure):
    _pack_ = 1
    ULTRASOUND, INFRARED = 0, 1
    _fields_ = [
        ("header", Header),
        ("radiation_type", ctypes.c_uint8),
        ("field_of_view", ctypes.c_float),
        ("min_range", ctypes.c_float),
        ("max_range", ctypes.c_float),
        ("range", ctypes.c_float),
    ]


class RegionOfInterest(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("x_offset", ctypes.c_uint32), ("y_offset", ctypes.c_uint32),
        ("height", ctypes.c_uint32), ("width", ctypes.c_uint32),
        ("do_rectify", ctypes.c_uint8),
    ]


class CameraInfo(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("header", Header),
        ("height", ctypes.c_uint32),
        ("width", ctypes.c_uint32),
        ("_distortion_model", ctypes.c_char * STRING_MAX_LEN),
        ("d_count", ctypes.c_uint32),
        ("D", ctypes.c_double * MAX_DISTORTION_COEFFS),
        ("K", ctypes.c_double * 9),
        ("R", ctypes.c_double * 9),
        ("P", ctypes.c_double * 12),
        ("binning_x", ctypes.c_uint32),
        ("binning_y", ctypes.c_uint32),
        ("roi", RegionOfInterest),
    ]

    def set_distortion_model(self, s: str):
        self._distortion_model = s.encode("utf-8")[:STRING_MAX_LEN - 1]

    def distortion_model_str(self) -> str:
        return self._distortion_model.rstrip(b"\x00").decode("utf-8", errors="replace")


class PointField(ctypes.LittleEndianStructure):
    _pack_ = 1
    INT8, UINT8, INT16, UINT16, INT32, UINT32, FLOAT32, FLOAT64 = 1, 2, 3, 4, 5, 6, 7, 8
    _fields_ = [
        ("_name", ctypes.c_char * STRING_MAX_LEN),
        ("offset", ctypes.c_uint32),
        ("datatype", ctypes.c_uint8),
        ("count", ctypes.c_uint32),
    ]

    def set_name(self, s: str):
        self._name = s.encode("utf-8")[:STRING_MAX_LEN - 1]

    def name_str(self) -> str:
        return self._name.rstrip(b"\x00").decode("utf-8", errors="replace")


class PointCloud2(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("header", Header),
        ("height", ctypes.c_uint32),
        ("width", ctypes.c_uint32),
        ("fields_count", ctypes.c_uint32),
        ("fields", PointField * MAX_POINT_FIELDS),
        ("is_bigendian", ctypes.c_uint8),
        ("point_step", ctypes.c_uint32),
        ("row_step", ctypes.c_uint32),
        ("data_len", ctypes.c_uint32),
        ("data", ctypes.c_uint8 * MAX_POINT_CLOUD_BYTES),
        ("is_dense", ctypes.c_uint8),
    ]


class LaserScan(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("header", Header),
        ("angle_min", ctypes.c_float), ("angle_max", ctypes.c_float), ("angle_increment", ctypes.c_float),
        ("time_increment", ctypes.c_float), ("scan_time", ctypes.c_float),
        ("range_min", ctypes.c_float), ("range_max", ctypes.c_float),
        ("ranges_count", ctypes.c_uint32),
        ("ranges", ctypes.c_float * MAX_LASER_SCAN_POINTS),
        ("intensities_count", ctypes.c_uint32),
        ("intensities", ctypes.c_float * MAX_LASER_SCAN_POINTS),
    ]


class JointState(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("header", Header),
        ("count", ctypes.c_uint32),
        ("_name", (ctypes.c_char * STRING_MAX_LEN) * MAX_JOINTS),
        ("position", ctypes.c_double * MAX_JOINTS),
        ("velocity", ctypes.c_double * MAX_JOINTS),
        ("effort", ctypes.c_double * MAX_JOINTS),
    ]

    def set_name(self, i: int, s: str):
        encoded = s.encode("utf-8")[:STRING_MAX_LEN - 1]
        self._name[i][:] = encoded.ljust(STRING_MAX_LEN, b"\x00")

    def name_str(self, i: int) -> str:
        return bytes(self._name[i]).rstrip(b"\x00").decode("utf-8", errors="replace")


for _t in (Imu, NavSatStatus, NavSatFix, MagneticField, Temperature, Range, RegionOfInterest,
           CameraInfo, PointField, PointCloud2, LaserScan, JointState):
    _check_fits(_t)


# --- nav_msgs ---------------------------------------------------------------

MAX_PATH_POSES = 16
MAX_OCCUPANCY_GRID_CELLS = 1024
MAX_GRID_CELLS = 64


class Odometry(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("header", Header),
        ("_child_frame_id", ctypes.c_char * FRAME_ID_MAX_LEN),
        ("pose", Pose),
        ("pose_covariance", ctypes.c_float * 36),
        ("twist", Twist),
        ("twist_covariance", ctypes.c_float * 36),
    ]

    def set_child_frame_id(self, name: str):
        self._child_frame_id = name.encode("utf-8")[:FRAME_ID_MAX_LEN - 1]

    def child_frame_id_str(self) -> str:
        return self._child_frame_id.rstrip(b"\x00").decode("utf-8", errors="replace")


class MapMetaData(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("map_load_time", Time), ("resolution", ctypes.c_float),
        ("width", ctypes.c_uint32), ("height", ctypes.c_uint32), ("origin", Pose),
    ]


class Path(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [("header", Header), ("count", ctypes.c_uint32), ("poses", PoseStamped * MAX_PATH_POSES)]


class OccupancyGrid(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("header", Header), ("info", MapMetaData),
        ("data_len", ctypes.c_uint32), ("data", ctypes.c_int8 * MAX_OCCUPANCY_GRID_CELLS),
    ]


class GridCells(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("header", Header), ("cell_width", ctypes.c_float), ("cell_height", ctypes.c_float),
        ("count", ctypes.c_uint32), ("cells", Point * MAX_GRID_CELLS),
    ]


for _t in (Odometry, MapMetaData, Path, OccupancyGrid, GridCells):
    _check_fits(_t)


# --- diagnostic_msgs --------------------------------------------------------

KV_MAX_LEN = 32
MAX_KEY_VALUES = 3
MAX_DIAGNOSTIC_STATUSES = 3


class KeyValue(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [("_key", ctypes.c_char * KV_MAX_LEN), ("_value", ctypes.c_char * KV_MAX_LEN)]

    def set_key(self, s: str):
        self._key = s.encode("utf-8")[:KV_MAX_LEN - 1]

    def set_value(self, s: str):
        self._value = s.encode("utf-8")[:KV_MAX_LEN - 1]

    def key_str(self) -> str:
        return self._key.rstrip(b"\x00").decode("utf-8", errors="replace")

    def value_str(self) -> str:
        return self._value.rstrip(b"\x00").decode("utf-8", errors="replace")


class DiagnosticStatus(ctypes.LittleEndianStructure):
    _pack_ = 1
    OK, WARN, ERROR, STALE = 0, 1, 2, 3
    _fields_ = [
        ("level", ctypes.c_int8),
        ("_name", ctypes.c_char * STRING_MAX_LEN),
        ("_message", ctypes.c_char * STRING_MAX_LEN),
        ("_hardware_id", ctypes.c_char * STRING_MAX_LEN),
        ("values_count", ctypes.c_uint32),
        ("values", KeyValue * MAX_KEY_VALUES),
    ]

    def set_name(self, s: str):
        self._name = s.encode("utf-8")[:STRING_MAX_LEN - 1]

    def set_message(self, s: str):
        self._message = s.encode("utf-8")[:STRING_MAX_LEN - 1]

    def set_hardware_id(self, s: str):
        self._hardware_id = s.encode("utf-8")[:STRING_MAX_LEN - 1]

    def name_str(self) -> str:
        return self._name.rstrip(b"\x00").decode("utf-8", errors="replace")

    def message_str(self) -> str:
        return self._message.rstrip(b"\x00").decode("utf-8", errors="replace")

    def hardware_id_str(self) -> str:
        return self._hardware_id.rstrip(b"\x00").decode("utf-8", errors="replace")


class DiagnosticArray(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("header", Header),
        ("status_count", ctypes.c_uint32),
        ("status", DiagnosticStatus * MAX_DIAGNOSTIC_STATUSES),
    ]


for _t in (KeyValue, DiagnosticStatus, DiagnosticArray):
    _check_fits(_t)


# --- trajectory_msgs --------------------------------------------------------

MAX_DOF = 4
MAX_TRAJECTORY_POINTS = 6
MAX_MULTI_DOF_TRAJECTORY_POINTS = 3


class JointTrajectoryPoint(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("count", ctypes.c_uint32),
        ("positions", ctypes.c_double * MAX_DOF),
        ("velocities", ctypes.c_double * MAX_DOF),
        ("accelerations", ctypes.c_double * MAX_DOF),
        ("effort", ctypes.c_double * MAX_DOF),
        ("time_from_start", Duration),
    ]


class JointTrajectory(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("header", Header),
        ("joint_names_count", ctypes.c_uint32),
        ("_joint_names", (ctypes.c_char * STRING_MAX_LEN) * MAX_DOF),
        ("points_count", ctypes.c_uint32),
        ("points", JointTrajectoryPoint * MAX_TRAJECTORY_POINTS),
    ]

    def set_joint_name(self, i: int, s: str):
        encoded = s.encode("utf-8")[:STRING_MAX_LEN - 1]
        self._joint_names[i][:] = encoded.ljust(STRING_MAX_LEN, b"\x00")


class MultiDOFJointTrajectoryPoint(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("count", ctypes.c_uint32),
        ("transforms", Transform * MAX_DOF),
        ("velocities", Twist * MAX_DOF),
        ("accelerations", Twist * MAX_DOF),
        ("time_from_start", Duration),
    ]


class MultiDOFJointTrajectory(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("header", Header),
        ("joint_names_count", ctypes.c_uint32),
        ("_joint_names", (ctypes.c_char * STRING_MAX_LEN) * MAX_DOF),
        ("points_count", ctypes.c_uint32),
        ("points", MultiDOFJointTrajectoryPoint * MAX_MULTI_DOF_TRAJECTORY_POINTS),
    ]

    def set_joint_name(self, i: int, s: str):
        encoded = s.encode("utf-8")[:STRING_MAX_LEN - 1]
        self._joint_names[i][:] = encoded.ljust(STRING_MAX_LEN, b"\x00")


for _t in (JointTrajectoryPoint, JointTrajectory, MultiDOFJointTrajectoryPoint, MultiDOFJointTrajectory):
    _check_fits(_t)


# --- actionlib_msgs -----------------------------------------------------------

MAX_GOAL_STATUSES = 8


class GoalID(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [("stamp", Time), ("_id", ctypes.c_char * STRING_MAX_LEN)]

    def set_id(self, s: str):
        self._id = s.encode("utf-8")[:STRING_MAX_LEN - 1]

    def id_str(self) -> str:
        return self._id.rstrip(b"\x00").decode("utf-8", errors="replace")


class GoalStatus(ctypes.LittleEndianStructure):
    _pack_ = 1
    (PENDING, ACTIVE, PREEMPTED, SUCCEEDED, ABORTED,
     REJECTED, PREEMPTING, RECALLING, RECALLED, LOST) = range(10)
    _fields_ = [("goal_id", GoalID), ("status", ctypes.c_uint8), ("_text", ctypes.c_char * STRING_MAX_LEN)]

    def set_text(self, s: str):
        self._text = s.encode("utf-8")[:STRING_MAX_LEN - 1]

    def text_str(self) -> str:
        return self._text.rstrip(b"\x00").decode("utf-8", errors="replace")


class GoalStatusArray(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("header", Header),
        ("status_list_count", ctypes.c_uint32),
        ("status_list", GoalStatus * MAX_GOAL_STATUSES),
    ]


for _t in (GoalID, GoalStatus, GoalStatusArray):
    _check_fits(_t)

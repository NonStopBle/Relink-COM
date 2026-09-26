// msg_schema.hpp -- rl_topic's C++ analog of rl_topic.py's msg_schema.py
// --type support, for `rl_topic echo`. C++ has no runtime reflection, so
// unlike Python's ctypes.Structure._fields_, every type's field layout
// here is hand-registered against relink/standard_msgs.hpp -- this file
// must be kept in sync with that header by hand. Custom --msg <file>
// schemas (msg_schema.py's other feature) are NOT implemented here; only
// built-in types are supported.
//
// Provides, mirroring msg_schema.py:
//   find_type_by_name(name)   -- explicit --type <Name> lookup
//   find_types_by_size(size)  -- every registered type whose exact wire
//                                 size matches, for size-based auto-
//                                 detect (see rl_topic.cpp's cmd_echo) --
//                                 the caller should only auto-decode when
//                                 this returns exactly one match, since
//                                 ReLink's wire format carries no type
//                                 tag and plenty of built-ins deliberately
//                                 share a size (Bool/Byte/Char/Int8/UInt8
//                                 are all 1 byte, Int32/UInt32/Float32
//                                 are all 4, etc.) -- more than one match
//                                 means genuinely ambiguous, not a bug.
//   format_message(desc, data) -- recursive pretty-print, one field per
//                                 line, nested structs/arrays indented.

#pragma once

#include "relink/standard_msgs.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace relink_msg_schema {

enum class FKind { I8, U8, I16, U16, I32, U32, I64, U64, F32, F64,
                    CHAR_STR, CHAR_STR_ARRAY, NUM_ARRAY, STRUCT, STRUCT_ARRAY };

struct TypeDesc; // fwd decl -- FieldDesc::nested points to one

struct FieldDesc {
    const char* name;
    FKind kind;
    size_t offset;
    // Meaning of count/elem_size/elem_kind/nested depends on `kind`:
    //   scalar (I8..F64)     : all four unused
    //   CHAR_STR             : elem_size = buffer length; count/elem_kind/nested unused
    //   CHAR_STR_ARRAY       : count = outer length, elem_size = inner buffer length per string
    //   NUM_ARRAY            : count = element count, elem_kind = each element's numeric kind
    //   STRUCT               : elem_size = sizeof(field), nested = its TypeDesc; count unused
    //   STRUCT_ARRAY         : count = element count, elem_size = sizeof(one element), nested = its TypeDesc
    size_t count = 1;
    size_t elem_size = 0;
    FKind elem_kind = FKind::U8;
    const TypeDesc* nested = nullptr;
};

struct TypeDesc {
    const char* name;
    size_t size;
    const FieldDesc* fields;
    size_t field_count;
};

// --------------------------------------------------------------------
// Registration macros -- offsetof()/sizeof(Struct::field) so a field
// getting renamed/resized in standard_msgs.hpp is a compile error here,
// not a silent mismatch (only the array LENGTHS/nested-type choices
// below are hand-matched against that header and won't be caught by the
// compiler if it changes).
// --------------------------------------------------------------------
#define RMS_SCALAR(S, F, K) \
    relink_msg_schema::FieldDesc{#F, relink_msg_schema::FKind::K, offsetof(S, F), 1, 0, relink_msg_schema::FKind::K, nullptr}
#define RMS_NUMARR(S, F, EK) \
    relink_msg_schema::FieldDesc{#F, relink_msg_schema::FKind::NUM_ARRAY, offsetof(S, F), \
        sizeof(S::F) / sizeof(S::F[0]), 0, relink_msg_schema::FKind::EK, nullptr}
#define RMS_STR(S, F) \
    relink_msg_schema::FieldDesc{#F, relink_msg_schema::FKind::CHAR_STR, offsetof(S, F), 1, sizeof(S::F), \
        relink_msg_schema::FKind::U8, nullptr}
#define RMS_STRARR(S, F) \
    relink_msg_schema::FieldDesc{#F, relink_msg_schema::FKind::CHAR_STR_ARRAY, offsetof(S, F), \
        sizeof(S::F) / sizeof(S::F[0]), sizeof(S::F[0]), relink_msg_schema::FKind::U8, nullptr}
#define RMS_STRUCT(S, F, DESC) \
    relink_msg_schema::FieldDesc{#F, relink_msg_schema::FKind::STRUCT, offsetof(S, F), 1, sizeof(S::F), \
        relink_msg_schema::FKind::U8, &(DESC)}
#define RMS_STRUCTARR(S, F, DESC) \
    relink_msg_schema::FieldDesc{#F, relink_msg_schema::FKind::STRUCT_ARRAY, offsetof(S, F), \
        sizeof(S::F) / sizeof(S::F[0]), sizeof(S::F[0]), relink_msg_schema::FKind::U8, &(DESC)}

#define RMS_TYPE(VARNAME, DISPLAYNAME, STYPE, ...) \
    static const relink_msg_schema::FieldDesc VARNAME##_fields[] = { __VA_ARGS__ }; \
    static const relink_msg_schema::TypeDesc VARNAME = { \
        DISPLAYNAME, sizeof(STYPE), VARNAME##_fields, \
        sizeof(VARNAME##_fields) / sizeof(VARNAME##_fields[0]) }

// --------------------------------------------------------------------
// Primitives (wire.hpp) -- each a single `data` field.
// --------------------------------------------------------------------
RMS_TYPE(kBool,    "Bool",    relink::Bool,    RMS_SCALAR(relink::Bool, data, U8));
RMS_TYPE(kByte,    "Byte",    relink::Byte,    RMS_SCALAR(relink::Byte, data, U8));
RMS_TYPE(kChar,    "Char",    relink::Char,    RMS_SCALAR(relink::Char, data, U8));
RMS_TYPE(kInt8,    "Int8",    relink::Int8,    RMS_SCALAR(relink::Int8, data, I8));
RMS_TYPE(kInt16,   "Int16",   relink::Int16,   RMS_SCALAR(relink::Int16, data, I16));
RMS_TYPE(kInt32,   "Int32",   relink::Int32,   RMS_SCALAR(relink::Int32, data, I32));
RMS_TYPE(kInt64,   "Int64",   relink::Int64,   RMS_SCALAR(relink::Int64, data, I64));
RMS_TYPE(kUInt8,   "UInt8",   relink::UInt8,   RMS_SCALAR(relink::UInt8, data, U8));
RMS_TYPE(kUInt16,  "UInt16",  relink::UInt16,  RMS_SCALAR(relink::UInt16, data, U16));
RMS_TYPE(kUInt32,  "UInt32",  relink::UInt32,  RMS_SCALAR(relink::UInt32, data, U32));
RMS_TYPE(kUInt64,  "UInt64",  relink::UInt64,  RMS_SCALAR(relink::UInt64, data, U64));
RMS_TYPE(kFloat32, "Float32", relink::Float32, RMS_SCALAR(relink::Float32, data, F32));
RMS_TYPE(kFloat64, "Float64", relink::Float64, RMS_SCALAR(relink::Float64, data, F64));

// --------------------------------------------------------------------
// std_msgs
// --------------------------------------------------------------------
using relink::std_msgs::Time;
using relink::std_msgs::Duration;
using relink::std_msgs::ColorRGBA;
using relink::std_msgs::Header;
using relink::std_msgs::String;

RMS_TYPE(kTime, "Time", Time,
    RMS_SCALAR(Time, sec, U32), RMS_SCALAR(Time, nsec, U32));
RMS_TYPE(kDuration, "Duration", Duration,
    RMS_SCALAR(Duration, sec, I32), RMS_SCALAR(Duration, nsec, I32));
RMS_TYPE(kColorRGBA, "ColorRGBA", ColorRGBA,
    RMS_SCALAR(ColorRGBA, r, F32), RMS_SCALAR(ColorRGBA, g, F32),
    RMS_SCALAR(ColorRGBA, b, F32), RMS_SCALAR(ColorRGBA, a, F32));
RMS_TYPE(kHeader, "Header", Header,
    RMS_SCALAR(Header, seq, U32), RMS_STRUCT(Header, stamp, kTime), RMS_STR(Header, frame_id));
RMS_TYPE(kString, "String", String, RMS_STR(String, data));

// --------------------------------------------------------------------
// geometry_msgs
// --------------------------------------------------------------------
using namespace relink::geometry_msgs; // Vector3, Point, Quaternion, Pose, ...

RMS_TYPE(kVector3, "Vector3", Vector3,
    RMS_SCALAR(Vector3, x, F32), RMS_SCALAR(Vector3, y, F32), RMS_SCALAR(Vector3, z, F32));
RMS_TYPE(kPoint, "Point", Point,
    RMS_SCALAR(Point, x, F32), RMS_SCALAR(Point, y, F32), RMS_SCALAR(Point, z, F32));
RMS_TYPE(kPoint32, "Point32", Point32,
    RMS_SCALAR(Point32, x, F32), RMS_SCALAR(Point32, y, F32), RMS_SCALAR(Point32, z, F32));
RMS_TYPE(kQuaternion, "Quaternion", Quaternion,
    RMS_SCALAR(Quaternion, x, F32), RMS_SCALAR(Quaternion, y, F32),
    RMS_SCALAR(Quaternion, z, F32), RMS_SCALAR(Quaternion, w, F32));
RMS_TYPE(kPose, "Pose", Pose,
    RMS_STRUCT(Pose, position, kPoint), RMS_STRUCT(Pose, orientation, kQuaternion));
RMS_TYPE(kTwist, "Twist", Twist,
    RMS_STRUCT(Twist, linear, kVector3), RMS_STRUCT(Twist, angular, kVector3));
RMS_TYPE(kAccel, "Accel", Accel,
    RMS_STRUCT(Accel, linear, kVector3), RMS_STRUCT(Accel, angular, kVector3));
RMS_TYPE(kWrench, "Wrench", Wrench,
    RMS_STRUCT(Wrench, force, kVector3), RMS_STRUCT(Wrench, torque, kVector3));
RMS_TYPE(kPoseStamped, "PoseStamped", PoseStamped,
    RMS_STRUCT(PoseStamped, header, kHeader), RMS_STRUCT(PoseStamped, pose, kPose));
RMS_TYPE(kTwistStamped, "TwistStamped", TwistStamped,
    RMS_STRUCT(TwistStamped, header, kHeader), RMS_STRUCT(TwistStamped, twist, kTwist));
RMS_TYPE(kTransform, "Transform", Transform,
    RMS_STRUCT(Transform, translation, kVector3), RMS_STRUCT(Transform, rotation, kQuaternion));
RMS_TYPE(kTransformStamped, "TransformStamped", TransformStamped,
    RMS_STRUCT(TransformStamped, header, kHeader), RMS_STR(TransformStamped, child_frame_id),
    RMS_STRUCT(TransformStamped, transform, kTransform));
RMS_TYPE(kPoseWithCovariance, "PoseWithCovariance", PoseWithCovariance,
    RMS_STRUCT(PoseWithCovariance, pose, kPose), RMS_NUMARR(PoseWithCovariance, covariance, F32));
RMS_TYPE(kTwistWithCovariance, "TwistWithCovariance", TwistWithCovariance,
    RMS_STRUCT(TwistWithCovariance, twist, kTwist), RMS_NUMARR(TwistWithCovariance, covariance, F32));
RMS_TYPE(kPoseArray, "PoseArray", PoseArray,
    RMS_STRUCT(PoseArray, header, kHeader), RMS_SCALAR(PoseArray, count, U32),
    RMS_STRUCTARR(PoseArray, poses, kPose));
RMS_TYPE(kPolygon, "Polygon", Polygon,
    RMS_SCALAR(Polygon, count, U32), RMS_STRUCTARR(Polygon, points, kPoint32));

// --------------------------------------------------------------------
// sensor_msgs (ImageChunk/CompressedImageChunk excluded -- chunked
// variable-length transfer, not a fixed-layout type meaningful to
// decode/auto-detect the way everything else on this page is)
// --------------------------------------------------------------------
using namespace relink::sensor_msgs;

RMS_TYPE(kImu, "Imu", Imu,
    RMS_STRUCT(Imu, header, kHeader), RMS_STRUCT(Imu, orientation, kQuaternion),
    RMS_NUMARR(Imu, orientation_covariance, F32), RMS_STRUCT(Imu, angular_velocity, kVector3),
    RMS_NUMARR(Imu, angular_velocity_covariance, F32), RMS_STRUCT(Imu, linear_acceleration, kVector3),
    RMS_NUMARR(Imu, linear_acceleration_covariance, F32));
RMS_TYPE(kNavSatStatus, "NavSatStatus", NavSatStatus,
    RMS_SCALAR(NavSatStatus, status, I8), RMS_SCALAR(NavSatStatus, service, U16));
RMS_TYPE(kNavSatFix, "NavSatFix", NavSatFix,
    RMS_STRUCT(NavSatFix, header, kHeader), RMS_STRUCT(NavSatFix, status, kNavSatStatus),
    RMS_SCALAR(NavSatFix, latitude, F64), RMS_SCALAR(NavSatFix, longitude, F64),
    RMS_SCALAR(NavSatFix, altitude, F64), RMS_NUMARR(NavSatFix, position_covariance, F64),
    RMS_SCALAR(NavSatFix, position_covariance_type, U8));
RMS_TYPE(kMagneticField, "MagneticField", MagneticField,
    RMS_STRUCT(MagneticField, header, kHeader), RMS_STRUCT(MagneticField, magnetic_field, kVector3),
    RMS_NUMARR(MagneticField, magnetic_field_covariance, F32));
RMS_TYPE(kTemperature, "Temperature", Temperature,
    RMS_STRUCT(Temperature, header, kHeader), RMS_SCALAR(Temperature, temperature, F64),
    RMS_SCALAR(Temperature, variance, F64));
RMS_TYPE(kRange, "Range", Range,
    RMS_STRUCT(Range, header, kHeader), RMS_SCALAR(Range, radiation_type, U8),
    RMS_SCALAR(Range, field_of_view, F32), RMS_SCALAR(Range, min_range, F32),
    RMS_SCALAR(Range, max_range, F32), RMS_SCALAR(Range, range, F32));
RMS_TYPE(kRegionOfInterest, "RegionOfInterest", RegionOfInterest,
    RMS_SCALAR(RegionOfInterest, x_offset, U32), RMS_SCALAR(RegionOfInterest, y_offset, U32),
    RMS_SCALAR(RegionOfInterest, height, U32), RMS_SCALAR(RegionOfInterest, width, U32),
    RMS_SCALAR(RegionOfInterest, do_rectify, U8));
RMS_TYPE(kCameraInfo, "CameraInfo", CameraInfo,
    RMS_STRUCT(CameraInfo, header, kHeader), RMS_SCALAR(CameraInfo, height, U32),
    RMS_SCALAR(CameraInfo, width, U32), RMS_STR(CameraInfo, distortion_model),
    RMS_SCALAR(CameraInfo, d_count, U32), RMS_NUMARR(CameraInfo, D, F64),
    RMS_NUMARR(CameraInfo, K, F64), RMS_NUMARR(CameraInfo, R, F64), RMS_NUMARR(CameraInfo, P, F64),
    RMS_SCALAR(CameraInfo, binning_x, U32), RMS_SCALAR(CameraInfo, binning_y, U32),
    RMS_STRUCT(CameraInfo, roi, kRegionOfInterest));
RMS_TYPE(kPointField, "PointField", PointField,
    RMS_STR(PointField, name), RMS_SCALAR(PointField, offset, U32),
    RMS_SCALAR(PointField, datatype, U8), RMS_SCALAR(PointField, count, U32));
RMS_TYPE(kPointCloud2, "PointCloud2", PointCloud2,
    RMS_STRUCT(PointCloud2, header, kHeader), RMS_SCALAR(PointCloud2, height, U32),
    RMS_SCALAR(PointCloud2, width, U32), RMS_SCALAR(PointCloud2, fields_count, U32),
    RMS_STRUCTARR(PointCloud2, fields, kPointField), RMS_SCALAR(PointCloud2, is_bigendian, U8),
    RMS_SCALAR(PointCloud2, point_step, U32), RMS_SCALAR(PointCloud2, row_step, U32),
    RMS_SCALAR(PointCloud2, data_len, U32), RMS_NUMARR(PointCloud2, data, U8),
    RMS_SCALAR(PointCloud2, is_dense, U8));
RMS_TYPE(kLaserScan, "LaserScan", LaserScan,
    RMS_STRUCT(LaserScan, header, kHeader), RMS_SCALAR(LaserScan, angle_min, F32),
    RMS_SCALAR(LaserScan, angle_max, F32), RMS_SCALAR(LaserScan, angle_increment, F32),
    RMS_SCALAR(LaserScan, time_increment, F32), RMS_SCALAR(LaserScan, scan_time, F32),
    RMS_SCALAR(LaserScan, range_min, F32), RMS_SCALAR(LaserScan, range_max, F32),
    RMS_SCALAR(LaserScan, ranges_count, U32), RMS_NUMARR(LaserScan, ranges, F32),
    RMS_SCALAR(LaserScan, intensities_count, U32), RMS_NUMARR(LaserScan, intensities, F32));
RMS_TYPE(kJointState, "JointState", JointState,
    RMS_STRUCT(JointState, header, kHeader), RMS_SCALAR(JointState, count, U32),
    RMS_STRARR(JointState, name), RMS_NUMARR(JointState, position, F64),
    RMS_NUMARR(JointState, velocity, F64), RMS_NUMARR(JointState, effort, F64));

// --------------------------------------------------------------------
// nav_msgs
// --------------------------------------------------------------------
using namespace relink::nav_msgs;

RMS_TYPE(kOdometry, "Odometry", Odometry,
    RMS_STRUCT(Odometry, header, kHeader), RMS_STR(Odometry, child_frame_id),
    RMS_STRUCT(Odometry, pose, kPose), RMS_NUMARR(Odometry, pose_covariance, F32),
    RMS_STRUCT(Odometry, twist, kTwist), RMS_NUMARR(Odometry, twist_covariance, F32));
RMS_TYPE(kMapMetaData, "MapMetaData", MapMetaData,
    RMS_STRUCT(MapMetaData, map_load_time, kTime), RMS_SCALAR(MapMetaData, resolution, F32),
    RMS_SCALAR(MapMetaData, width, U32), RMS_SCALAR(MapMetaData, height, U32),
    RMS_STRUCT(MapMetaData, origin, kPose));
RMS_TYPE(kPath, "Path", Path,
    RMS_STRUCT(Path, header, kHeader), RMS_SCALAR(Path, count, U32),
    RMS_STRUCTARR(Path, poses, kPoseStamped));
RMS_TYPE(kOccupancyGrid, "OccupancyGrid", OccupancyGrid,
    RMS_STRUCT(OccupancyGrid, header, kHeader), RMS_STRUCT(OccupancyGrid, info, kMapMetaData),
    RMS_SCALAR(OccupancyGrid, data_len, U32), RMS_NUMARR(OccupancyGrid, data, I8));
RMS_TYPE(kGridCells, "GridCells", GridCells,
    RMS_STRUCT(GridCells, header, kHeader), RMS_SCALAR(GridCells, cell_width, F32),
    RMS_SCALAR(GridCells, cell_height, F32), RMS_SCALAR(GridCells, count, U32),
    RMS_STRUCTARR(GridCells, cells, kPoint));

// --------------------------------------------------------------------
// diagnostic_msgs
// --------------------------------------------------------------------
using namespace relink::diagnostic_msgs;

RMS_TYPE(kKeyValue, "KeyValue", KeyValue, RMS_STR(KeyValue, key), RMS_STR(KeyValue, value));
RMS_TYPE(kDiagnosticStatus, "DiagnosticStatus", DiagnosticStatus,
    RMS_SCALAR(DiagnosticStatus, level, I8), RMS_STR(DiagnosticStatus, name),
    RMS_STR(DiagnosticStatus, message), RMS_STR(DiagnosticStatus, hardware_id),
    RMS_SCALAR(DiagnosticStatus, values_count, U32), RMS_STRUCTARR(DiagnosticStatus, values, kKeyValue));
RMS_TYPE(kDiagnosticArray, "DiagnosticArray", DiagnosticArray,
    RMS_STRUCT(DiagnosticArray, header, kHeader), RMS_SCALAR(DiagnosticArray, status_count, U32),
    RMS_STRUCTARR(DiagnosticArray, status, kDiagnosticStatus));

// --------------------------------------------------------------------
// trajectory_msgs
// --------------------------------------------------------------------
using namespace relink::trajectory_msgs;

RMS_TYPE(kJointTrajectoryPoint, "JointTrajectoryPoint", JointTrajectoryPoint,
    RMS_SCALAR(JointTrajectoryPoint, count, U32), RMS_NUMARR(JointTrajectoryPoint, positions, F64),
    RMS_NUMARR(JointTrajectoryPoint, velocities, F64), RMS_NUMARR(JointTrajectoryPoint, accelerations, F64),
    RMS_NUMARR(JointTrajectoryPoint, effort, F64), RMS_STRUCT(JointTrajectoryPoint, time_from_start, kDuration));
RMS_TYPE(kJointTrajectory, "JointTrajectory", JointTrajectory,
    RMS_STRUCT(JointTrajectory, header, kHeader), RMS_SCALAR(JointTrajectory, joint_names_count, U32),
    RMS_STRARR(JointTrajectory, joint_names), RMS_SCALAR(JointTrajectory, points_count, U32),
    RMS_STRUCTARR(JointTrajectory, points, kJointTrajectoryPoint));
RMS_TYPE(kMultiDOFJointTrajectoryPoint, "MultiDOFJointTrajectoryPoint", MultiDOFJointTrajectoryPoint,
    RMS_SCALAR(MultiDOFJointTrajectoryPoint, count, U32),
    RMS_STRUCTARR(MultiDOFJointTrajectoryPoint, transforms, kTransform),
    RMS_STRUCTARR(MultiDOFJointTrajectoryPoint, velocities, kTwist),
    RMS_STRUCTARR(MultiDOFJointTrajectoryPoint, accelerations, kTwist),
    RMS_STRUCT(MultiDOFJointTrajectoryPoint, time_from_start, kDuration));
RMS_TYPE(kMultiDOFJointTrajectory, "MultiDOFJointTrajectory", MultiDOFJointTrajectory,
    RMS_STRUCT(MultiDOFJointTrajectory, header, kHeader),
    RMS_SCALAR(MultiDOFJointTrajectory, joint_names_count, U32),
    RMS_STRARR(MultiDOFJointTrajectory, joint_names),
    RMS_SCALAR(MultiDOFJointTrajectory, points_count, U32),
    RMS_STRUCTARR(MultiDOFJointTrajectory, points, kMultiDOFJointTrajectoryPoint));

// --------------------------------------------------------------------
// actionlib_msgs
// --------------------------------------------------------------------
using namespace relink::actionlib_msgs;

RMS_TYPE(kGoalID, "GoalID", GoalID, RMS_STRUCT(GoalID, stamp, kTime), RMS_STR(GoalID, id));
RMS_TYPE(kGoalStatus, "GoalStatus", GoalStatus,
    RMS_STRUCT(GoalStatus, goal_id, kGoalID), RMS_SCALAR(GoalStatus, status, U8),
    RMS_STR(GoalStatus, text));
RMS_TYPE(kGoalStatusArray, "GoalStatusArray", GoalStatusArray,
    RMS_STRUCT(GoalStatusArray, header, kHeader), RMS_SCALAR(GoalStatusArray, status_list_count, U32),
    RMS_STRUCTARR(GoalStatusArray, status_list, kGoalStatus));

#undef RMS_SCALAR
#undef RMS_NUMARR
#undef RMS_STR
#undef RMS_STRARR
#undef RMS_STRUCT
#undef RMS_STRUCTARR
#undef RMS_TYPE

// --------------------------------------------------------------------
// Registry -- every type above, for find_type_by_name()/find_types_by_size().
// --------------------------------------------------------------------
inline const std::vector<const TypeDesc*>& all_types() {
    static const std::vector<const TypeDesc*> types = {
        &kBool, &kByte, &kChar, &kInt8, &kInt16, &kInt32, &kInt64,
        &kUInt8, &kUInt16, &kUInt32, &kUInt64, &kFloat32, &kFloat64,
        &kTime, &kDuration, &kColorRGBA, &kHeader, &kString,
        &kVector3, &kPoint, &kPoint32, &kQuaternion, &kPose, &kTwist, &kAccel, &kWrench,
        &kPoseStamped, &kTwistStamped, &kTransform, &kTransformStamped,
        &kPoseWithCovariance, &kTwistWithCovariance, &kPoseArray, &kPolygon,
        &kImu, &kNavSatStatus, &kNavSatFix, &kMagneticField, &kTemperature, &kRange,
        &kRegionOfInterest, &kCameraInfo, &kPointField, &kPointCloud2, &kLaserScan, &kJointState,
        &kOdometry, &kMapMetaData, &kPath, &kOccupancyGrid, &kGridCells,
        &kKeyValue, &kDiagnosticStatus, &kDiagnosticArray,
        &kJointTrajectoryPoint, &kJointTrajectory,
        &kMultiDOFJointTrajectoryPoint, &kMultiDOFJointTrajectory,
        &kGoalID, &kGoalStatus, &kGoalStatusArray,
    };
    return types;
}

inline const TypeDesc* find_type_by_name(const std::string& name) {
    for (const TypeDesc* t : all_types()) if (name == t->name) return t;
    return nullptr;
}

// See this file's header comment: caller should only auto-decode when
// this returns exactly one match.
inline std::vector<const TypeDesc*> find_types_by_size(size_t size) {
    std::vector<const TypeDesc*> out;
    for (const TypeDesc* t : all_types()) if (t->size == size) out.push_back(t);
    return out;
}

// --------------------------------------------------------------------
// Recursive pretty-printer -- mirrors msg_schema.py's format_message().
// --------------------------------------------------------------------
inline std::string format_message(const TypeDesc* desc, const uint8_t* data, int indent = 0) {
    std::string pad(static_cast<size_t>(indent + 1) * 2, ' ');
    std::string out;
    for (size_t i = 0; i < desc->field_count; ++i) {
        const FieldDesc& fd = desc->fields[i];
        const uint8_t* fp = data + fd.offset;
        out += pad + fd.name;
        switch (fd.kind) {
            case FKind::I8:  out += " = " + std::to_string(*reinterpret_cast<const int8_t*>(fp)); break;
            case FKind::U8:  out += " = " + std::to_string(*reinterpret_cast<const uint8_t*>(fp)); break;
            case FKind::I16: out += " = " + std::to_string(*reinterpret_cast<const int16_t*>(fp)); break;
            case FKind::U16: out += " = " + std::to_string(*reinterpret_cast<const uint16_t*>(fp)); break;
            case FKind::I32: out += " = " + std::to_string(*reinterpret_cast<const int32_t*>(fp)); break;
            case FKind::U32: out += " = " + std::to_string(*reinterpret_cast<const uint32_t*>(fp)); break;
            case FKind::I64: out += " = " + std::to_string(*reinterpret_cast<const int64_t*>(fp)); break;
            case FKind::U64: out += " = " + std::to_string(*reinterpret_cast<const uint64_t*>(fp)); break;
            case FKind::F32: out += " = " + std::to_string(*reinterpret_cast<const float*>(fp)); break;
            case FKind::F64: out += " = " + std::to_string(*reinterpret_cast<const double*>(fp)); break;
            case FKind::CHAR_STR: {
                const char* s = reinterpret_cast<const char*>(fp);
                out += " = \"" + std::string(s, strnlen(s, fd.elem_size)) + "\"";
                break;
            }
            case FKind::CHAR_STR_ARRAY: {
                out += " = [";
                for (size_t j = 0; j < fd.count; ++j) {
                    const char* s = reinterpret_cast<const char*>(fp) + j * fd.elem_size;
                    if (j) out += ", ";
                    out += "\"" + std::string(s, strnlen(s, fd.elem_size)) + "\"";
                }
                out += "]";
                break;
            }
            case FKind::NUM_ARRAY: {
                out += " = [";
                for (size_t j = 0; j < fd.count; ++j) {
                    if (j) out += ", ";
                    switch (fd.elem_kind) {
                        case FKind::I8:  out += std::to_string(reinterpret_cast<const int8_t*>(fp)[j]); break;
                        case FKind::U8:  out += std::to_string(reinterpret_cast<const uint8_t*>(fp)[j]); break;
                        case FKind::I16: out += std::to_string(reinterpret_cast<const int16_t*>(fp)[j]); break;
                        case FKind::U16: out += std::to_string(reinterpret_cast<const uint16_t*>(fp)[j]); break;
                        case FKind::I32: out += std::to_string(reinterpret_cast<const int32_t*>(fp)[j]); break;
                        case FKind::U32: out += std::to_string(reinterpret_cast<const uint32_t*>(fp)[j]); break;
                        case FKind::I64: out += std::to_string(reinterpret_cast<const int64_t*>(fp)[j]); break;
                        case FKind::U64: out += std::to_string(reinterpret_cast<const uint64_t*>(fp)[j]); break;
                        case FKind::F32: out += std::to_string(reinterpret_cast<const float*>(fp)[j]); break;
                        case FKind::F64: out += std::to_string(reinterpret_cast<const double*>(fp)[j]); break;
                        default: break;
                    }
                }
                out += "]";
                break;
            }
            case FKind::STRUCT:
                out += ":\n" + format_message(fd.nested, fp, indent + 1);
                break;
            case FKind::STRUCT_ARRAY: {
                out += ":\n";
                std::string pad2(static_cast<size_t>(indent + 2) * 2, ' ');
                for (size_t j = 0; j < fd.count; ++j) {
                    out += pad2 + "[" + std::to_string(j) + "]:\n";
                    out += format_message(fd.nested, fp + j * fd.elem_size, indent + 2);
                    if (j + 1 < fd.count) out += "\n";
                }
                break;
            }
        }
        if (i + 1 < desc->field_count) out += "\n";
    }
    return out;
}

} // namespace relink_msg_schema

// Standard message types -- ROS-familiar names/shapes (std_msgs,
// geometry_msgs, sensor_msgs, nav_msgs), reimplemented as ReLink's
// fixed-layout, trivially-copyable structs so advertise<T>/subscribe<T>/
// publish<T> just work on them like any other type (see wire.hpp's
// Bool/Int32/Float32/... for the primitives these build on).
//
// Two deliberate departures from ROS's actual wire format, both direct
// consequences of ReLink's fixed-size, no-heap, no-schema design (see
// the root README's "What ReLink actually is"):
//
//   1. No variable-length fields. ROS's std_msgs/String and
//      std_msgs/Header's `string frame_id` are heap-allocated,
//      arbitrary-length strings -- ReLink has no such type (a
//      length-prefixed field would defeat the fixed-size wire format
//      the rest of the library relies on). Header::frame_id here is a
//      fixed kFrameIdMaxLen-byte buffer instead; longer names are
//      truncated, not rejected -- see Header::set_frame_id().
//
//   2. float (32-bit) instead of ROS's default float64 for
//      geometry_msgs types (Vector3, Point, Quaternion, Pose, Twist,
//      Accel). ReLink's whole design point is the smallest reasonable
//      wire size for a real-time control loop (see the root README's
//      Benchmarks) -- halving these fields' size is exactly the kind
//      of tradeoff this library exists to make. NavSatFix keeps
//      double for latitude/longitude/altitude, where float32's ~7
//      decimal digits of precision (~1cm at the equator) isn't
//      obviously enough and ROS's own default reflects that same
//      judgment call.
//
//   3. No variable-length arrays either, for the same reason as #1.
//      ROS's `Pose[]`, `float64[]`, `string[]`, etc. become a fixed-
//      capacity array plus a `count` field (e.g. PoseArray::poses,
//      LaserScan::ranges) -- entries beyond the compile-time cap are
//      never stored. Every cap is a named `kMax...` constant declared
//      right next to the type it bounds, chosen to comfortably cover
//      typical use (16 joints, 360 laser points, etc.), not ROS's true
//      unbounded length. sensor_msgs::PointCloud2 and
//      nav_msgs::OccupancyGrid in particular cap far below realistic
//      ROS payloads (a real point cloud or map is easily megabytes) --
//      for those, chunk the way sensor_msgs::ImageChunk already does
//      for images, rather than relying on the fixed cap here.
//
// Every type is named and shaped after its ROS counterpart specifically
// so porting a ROS node's message usage to ReLink is a rename, not a
// redesign.

#pragma once

#include "relink/wire.hpp"
#include "relink/image.hpp"
#include "relink/frame.hpp" // kMaxPayloadBytes -- see the per-type static_asserts below
#include <cstdint>
#include <cstring>
#include <ctime>

namespace relink {

namespace std_msgs {

#pragma pack(push, 1)

// std_msgs/Empty -- zero-byte marker message (e.g. a "tick"/trigger topic).
struct Empty {};

// std_msgs/Time -- wall-clock timestamp, POSIX (sec, nsec) pair.
struct Time {
    uint32_t sec;
    uint32_t nsec;

    static Time now() {
        struct timespec ts{};
        ::clock_gettime(CLOCK_REALTIME, &ts);
        return Time{static_cast<uint32_t>(ts.tv_sec), static_cast<uint32_t>(ts.tv_nsec)};
    }
};

// std_msgs/Duration -- signed (sec, nsec) pair, e.g. an elapsed time.
struct Duration {
    int32_t sec;
    int32_t nsec;
};

// std_msgs/ColorRGBA -- each channel 0.0-1.0, per ROS convention.
struct ColorRGBA {
    float r;
    float g;
    float b;
    float a;
};

inline constexpr size_t kFrameIdMaxLen = 32;

// std_msgs/Header -- seq counter, timestamp, and a coordinate-frame
// name. frame_id is fixed-size (see the file-level comment above);
// use set_frame_id()/frame_id_str() rather than touching the raw
// buffer, so truncation and null-termination are handled consistently.
struct Header {
    uint32_t seq;
    Time stamp;
    char frame_id[kFrameIdMaxLen]; // not guaranteed null-terminated if exactly kFrameIdMaxLen chars

    void set_frame_id(const char* name) {
        std::memset(frame_id, 0, sizeof(frame_id));
        std::strncpy(frame_id, name, sizeof(frame_id) - 1);
    }
    // Always returns a null-terminated view, even if the stored name
    // filled the buffer exactly (memcpy + explicit terminator, since
    // strncpy alone doesn't guarantee termination in that case).
    std::string frame_id_str() const {
        char buf[kFrameIdMaxLen + 1];
        std::memcpy(buf, frame_id, kFrameIdMaxLen);
        buf[kFrameIdMaxLen] = '\0';
        return std::string(buf);
    }
    void stamp_now() { stamp = Time::now(); }
};

#pragma pack(pop)

inline constexpr size_t kStringMaxLen = 64;

// std_msgs/String -- fixed kStringMaxLen-byte buffer, not a dynamic
// string (see the file-level comment) -- use set()/str() rather than
// touching data directly, same reasoning as Header::frame_id.
#pragma pack(push, 1)
struct String {
    char data[kStringMaxLen];

    void set(const char* s) {
        std::memset(data, 0, sizeof(data));
        std::strncpy(data, s, sizeof(data) - 1);
    }
    std::string str() const {
        char buf[kStringMaxLen + 1];
        std::memcpy(buf, data, kStringMaxLen);
        buf[kStringMaxLen] = '\0';
        return std::string(buf);
    }
};
#pragma pack(pop)

static_assert(sizeof(Empty) == 1, "Empty should be the smallest possible struct (compilers can't do 0)");
static_assert(sizeof(Time) == 8, "Time must be 8 bytes");
static_assert(sizeof(Duration) == 8, "Duration must be 8 bytes");
static_assert(sizeof(ColorRGBA) == 16, "ColorRGBA must be 16 bytes");
static_assert(sizeof(Header) == 4 + 8 + kFrameIdMaxLen, "Header size must match its fields exactly");
static_assert(sizeof(String) == kStringMaxLen, "String must be exactly kStringMaxLen bytes");
static_assert(std::is_trivially_copyable<Header>::value, "Header must stay trivially copyable");
static_assert(std::is_trivially_copyable<String>::value, "String must stay trivially copyable");

} // namespace std_msgs

namespace geometry_msgs {

#pragma pack(push, 1)

// geometry_msgs/Vector3 -- see file-level comment: float32, not ROS's float64.
struct Vector3 {
    float x;
    float y;
    float z;
};

// geometry_msgs/Point -- identical layout to Vector3; kept as a
// separate type (not a using-alias) so a function signature makes the
// semantic distinction (a position vs. a direction/velocity) visible,
// matching ROS's own modeling choice.
struct Point {
    float x;
    float y;
    float z;
};

struct Quaternion {
    float x;
    float y;
    float z;
    float w;
};

struct Pose {
    Point position;
    Quaternion orientation;
};

struct Twist {
    Vector3 linear;
    Vector3 angular;
};

struct Accel {
    Vector3 linear;
    Vector3 angular;
};

struct Wrench {
    Vector3 force;
    Vector3 torque;
};

struct PoseStamped {
    std_msgs::Header header;
    Pose pose;
};

struct TwistStamped {
    std_msgs::Header header;
    Twist twist;
};

// geometry_msgs/Point32 -- same shape as Point, distinct ROS type (used
// where the smaller wire size matters, e.g. Polygon below).
struct Point32 {
    float x;
    float y;
    float z;
};

struct Transform {
    Vector3 translation;
    Quaternion rotation;
};

struct TransformStamped {
    std_msgs::Header header;
    char child_frame_id[std_msgs::kFrameIdMaxLen];
    Transform transform;

    void set_child_frame_id(const char* name) {
        std::memset(child_frame_id, 0, sizeof(child_frame_id));
        std::strncpy(child_frame_id, name, sizeof(child_frame_id) - 1);
    }
    std::string child_frame_id_str() const {
        char buf[std_msgs::kFrameIdMaxLen + 1];
        std::memcpy(buf, child_frame_id, std_msgs::kFrameIdMaxLen);
        buf[std_msgs::kFrameIdMaxLen] = '\0';
        return std::string(buf);
    }
};

struct PoseWithCovariance {
    Pose pose;
    float covariance[36]; // row-major 6x6, per ROS -- float32 not ROS's float64, see file header
};

struct TwistWithCovariance {
    Twist twist;
    float covariance[36];
};

// geometry_msgs/PoseArray, Polygon -- ROS's `Pose[]`/`Point32[]` become a
// fixed-capacity array + count (see the file-level comment's departure
// #3): entries beyond the cap are simply never stored -- check `count`
// against the cap constant if you need to detect that, this does not
// throw or truncate loudly the way Header/String's fixed strings do,
// since silently dropping array elements has no equivalent to a
// null-terminator convention to signal it happened.
inline constexpr uint32_t kMaxPosesInArray = 16;
struct PoseArray {
    std_msgs::Header header;
    uint32_t count; // number of valid entries in poses[0..count), <= kMaxPosesInArray
    Pose poses[kMaxPosesInArray];
};

inline constexpr uint32_t kMaxPolygonPoints = 16;
struct Polygon {
    uint32_t count;
    Point32 points[kMaxPolygonPoints];
};

#pragma pack(pop)

static_assert(sizeof(Vector3) == 12, "Vector3 must be 12 bytes (3x float32)");
static_assert(sizeof(Point) == 12, "Point must be 12 bytes (3x float32)");
static_assert(sizeof(Point32) == 12, "Point32 must be 12 bytes (3x float32)");
static_assert(sizeof(Quaternion) == 16, "Quaternion must be 16 bytes (4x float32)");
static_assert(sizeof(Pose) == sizeof(Point) + sizeof(Quaternion), "Pose must be Point+Quaternion, no padding");
static_assert(sizeof(Twist) == 2 * sizeof(Vector3), "Twist must be 2x Vector3, no padding");
static_assert(std::is_trivially_copyable<Pose>::value, "Pose must stay trivially copyable");
static_assert(std::is_trivially_copyable<PoseStamped>::value, "PoseStamped must stay trivially copyable");
static_assert(std::is_trivially_copyable<PoseArray>::value, "PoseArray must stay trivially copyable");
static_assert(std::is_trivially_copyable<Polygon>::value, "Polygon must stay trivially copyable");
static_assert(std::is_trivially_copyable<TransformStamped>::value, "TransformStamped must stay trivially copyable");

static_assert(sizeof(PoseArray) <= kMaxPayloadBytes, "PoseArray exceeds one UDP datagram -- shrink kMaxPosesInArray");
static_assert(sizeof(Polygon) <= kMaxPayloadBytes, "Polygon exceeds one UDP datagram -- shrink kMaxPolygonPoints");

} // namespace geometry_msgs

namespace sensor_msgs {

// sensor_msgs/Image / CompressedImage equivalent: ReLink's ImageChunk
// (image.hpp) -- a chunked large-blob type used via advertise_image/
// publish_image/subscribe_image on RelinkNode, NOT the plain advertise/
// subscribe/publish<T> the other types on this page use (a full image
// doesn't fit one UDP datagram, so it needs the chunk+reassemble
// machinery those dedicated methods provide -- see the root README's
// "Sending images" section).
using ImageChunk = ::relink::ImageChunk;

#pragma pack(push, 1)

// sensor_msgs/Imu -- orientation + angular velocity + linear
// acceleration, each with a 3x3 covariance matrix, exactly ROS's Imu
// shape (float32 throughout, see the file-level comment).
struct Imu {
    std_msgs::Header header;
    geometry_msgs::Quaternion orientation;
    float orientation_covariance[9];
    geometry_msgs::Vector3 angular_velocity;
    float angular_velocity_covariance[9];
    geometry_msgs::Vector3 linear_acceleration;
    float linear_acceleration_covariance[9];
};

// sensor_msgs/NavSatStatus
struct NavSatStatus {
    static constexpr int8_t kNoFix = -1, kFix = 0, kSbasFix = 1, kGbasFix = 2;
    static constexpr uint16_t kServiceGps = 1, kServiceGlonass = 2, kServiceCompass = 4, kServiceGalileo = 8;
    int8_t status;
    uint16_t service; // bitmask of the kService* constants above
};

// sensor_msgs/NavSatFix -- GPS fix. Keeps double for lat/lon/altitude
// (see file-level comment on precision).
struct NavSatFix {
    static constexpr uint8_t kCovarianceUnknown = 0, kCovarianceApproximated = 1,
                              kCovarianceDiagonalKnown = 2, kCovarianceKnown = 3;
    std_msgs::Header header;
    NavSatStatus status;
    double latitude;
    double longitude;
    double altitude;
    double position_covariance[9];
    uint8_t position_covariance_type;
};

// sensor_msgs/MagneticField -- Tesla, per ROS convention.
struct MagneticField {
    std_msgs::Header header;
    geometry_msgs::Vector3 magnetic_field;
    float magnetic_field_covariance[9]; // float32, see file-level comment
};

// sensor_msgs/Temperature -- degrees Celsius, per ROS convention.
struct Temperature {
    std_msgs::Header header;
    double temperature;
    double variance; // 0 means "unknown"
};

// sensor_msgs/Range -- a single-beam range reading (ultrasonic/IR/ToF).
struct Range {
    static constexpr uint8_t kUltrasound = 0, kInfrared = 1;
    std_msgs::Header header;
    uint8_t radiation_type;
    float field_of_view;    // radians
    float min_range;        // meters
    float max_range;        // meters
    float range;            // meters
};

// sensor_msgs/RegionOfInterest
struct RegionOfInterest {
    uint32_t x_offset;
    uint32_t y_offset;
    uint32_t height;
    uint32_t width;
    uint8_t do_rectify; // bool, stored as a byte on the wire (see wire.hpp's Bool)
};

inline constexpr uint32_t kMaxDistortionCoeffs = 8; // ROS's plumb_bob/rational_polynomial use 5-8

// sensor_msgs/CameraInfo -- distortion_model is a fixed string (see file
// header); D is a fixed-capacity array (see PoseArray's comment on that
// pattern) since ROS leaves its length model-dependent.
struct CameraInfo {
    std_msgs::Header header;
    uint32_t height;
    uint32_t width;
    char distortion_model[std_msgs::kStringMaxLen];
    uint32_t d_count; // valid entries in D[0..d_count), <= kMaxDistortionCoeffs
    double D[kMaxDistortionCoeffs];
    double K[9];
    double R[9];
    double P[12];
    uint32_t binning_x;
    uint32_t binning_y;
    RegionOfInterest roi;

    void set_distortion_model(const char* name) {
        std::memset(distortion_model, 0, sizeof(distortion_model));
        std::strncpy(distortion_model, name, sizeof(distortion_model) - 1);
    }
    std::string distortion_model_str() const {
        char buf[std_msgs::kStringMaxLen + 1];
        std::memcpy(buf, distortion_model, std_msgs::kStringMaxLen);
        buf[std_msgs::kStringMaxLen] = '\0';
        return std::string(buf);
    }
};

// sensor_msgs/PointField
struct PointField {
    static constexpr uint8_t kInt8 = 1, kUint8 = 2, kInt16 = 3, kUint16 = 4,
                              kInt32 = 5, kUint32 = 6, kFloat32 = 7, kFloat64 = 8;
    char name[std_msgs::kStringMaxLen];
    uint32_t offset;
    uint8_t datatype;
    uint32_t count;

    void set_name(const char* s) {
        std::memset(name, 0, sizeof(name));
        std::strncpy(name, s, sizeof(name) - 1);
    }
    std::string name_str() const {
        char buf[std_msgs::kStringMaxLen + 1];
        std::memcpy(buf, name, std_msgs::kStringMaxLen);
        buf[std_msgs::kStringMaxLen] = '\0';
        return std::string(buf);
    }
};

// kMaxPointFields/kMaxPointCloudBytes are small enough that PointCloud2
// (below) fits ReLink's single-UDP-datagram budget for plain
// advertise<T>/publish<T> (kMaxPayloadBytes, frame.hpp) -- see the
// static_assert after the struct. This is a MUCH smaller cap than a
// real point cloud (see the file-level comment #3): 4 fields covers a
// typical x/y/z/rgb cloud, 900 bytes covers ~75 points at 12 bytes/pt.
inline constexpr uint32_t kMaxPointFields = 4;
inline constexpr uint32_t kMaxPointCloudBytes = 900;

// sensor_msgs/PointCloud2 -- ROS's `uint8[] data` (typically megabytes
// for a real point cloud) becomes a small fixed cap here, NOT a
// production-ready ReLink point-cloud path -- for real point cloud
// volumes, chunk manually the way ImageChunk does for images (see
// sensor_msgs::ImageChunk above), or advertise/subscribe per-point
// instead of batching a whole cloud into one message.
struct PointCloud2 {
    std_msgs::Header header;
    uint32_t height;
    uint32_t width;
    uint32_t fields_count;
    PointField fields[kMaxPointFields];
    uint8_t is_bigendian;
    uint32_t point_step;
    uint32_t row_step;
    uint32_t data_len; // valid bytes in data[0..data_len), <= kMaxPointCloudBytes
    uint8_t data[kMaxPointCloudBytes];
    uint8_t is_dense;
};

// 120 points (~3 degrees/point over a full circle) -- 360 (1 deg/point,
// a common ROS default) doesn't fit ReLink's single-datagram budget
// alongside LaserScan's other fields (see the static_assert below); a
// full-resolution scan needs multiple messages or a chunked transfer.
inline constexpr uint32_t kMaxLaserScanPoints = 120;

// sensor_msgs/LaserScan -- ROS's `float32[] ranges`/`intensities` become
// fixed kMaxLaserScanPoints-capacity arrays (see PoseArray's comment).
struct LaserScan {
    std_msgs::Header header;
    float angle_min, angle_max, angle_increment;
    float time_increment, scan_time, range_min, range_max;
    uint32_t ranges_count;
    float ranges[kMaxLaserScanPoints];
    uint32_t intensities_count;
    float intensities[kMaxLaserScanPoints];
};

// 8, not a rounder 16 -- 16 doesn't fit ReLink's single-datagram budget
// alongside JointState's per-joint position/velocity/effort arrays (see
// the static_assert below).
inline constexpr uint32_t kMaxJoints = 8;

// sensor_msgs/JointState -- ROS's `string[] name` becomes a fixed array
// of fixed strings; position/velocity/effort become fixed-capacity
// float arrays (see PoseArray's comment on the general pattern). All
// four share one `count` since ROS itself requires them to be either
// empty or the same length as `name`.
struct JointState {
    std_msgs::Header header;
    uint32_t count; // valid entries in every array below, <= kMaxJoints
    char name[kMaxJoints][std_msgs::kStringMaxLen];
    double position[kMaxJoints];
    double velocity[kMaxJoints];
    double effort[kMaxJoints];

    void set_name(uint32_t i, const char* s) {
        std::memset(name[i], 0, std_msgs::kStringMaxLen);
        std::strncpy(name[i], s, std_msgs::kStringMaxLen - 1);
    }
    std::string name_str(uint32_t i) const {
        char buf[std_msgs::kStringMaxLen + 1];
        std::memcpy(buf, name[i], std_msgs::kStringMaxLen);
        buf[std_msgs::kStringMaxLen] = '\0';
        return std::string(buf);
    }
};

#pragma pack(pop)

static_assert(std::is_trivially_copyable<Imu>::value, "Imu must stay trivially copyable");
static_assert(std::is_trivially_copyable<NavSatFix>::value, "NavSatFix must stay trivially copyable");
static_assert(std::is_trivially_copyable<MagneticField>::value, "MagneticField must stay trivially copyable");
static_assert(std::is_trivially_copyable<CameraInfo>::value, "CameraInfo must stay trivially copyable");
static_assert(std::is_trivially_copyable<PointCloud2>::value, "PointCloud2 must stay trivially copyable");
static_assert(std::is_trivially_copyable<LaserScan>::value, "LaserScan must stay trivially copyable");
static_assert(std::is_trivially_copyable<JointState>::value, "JointState must stay trivially copyable");

// Every plain (non-chunked, i.e. not ImageChunk) type in this file must
// fit ReLink's single-UDP-datagram budget for advertise<T>/publish<T> --
// otherwise publish<T> silently returns false (per the wire size check
// in relink.hpp), which is exactly the kind of quiet failure this
// library's "never misinterpret/drop without saying so" rule exists to
// prevent. These asserts catch a too-large type at COMPILE time
// instead, for anyone tempted to raise one of the kMax* caps above.
static_assert(sizeof(CameraInfo) <= kMaxPayloadBytes, "CameraInfo exceeds one UDP datagram");
static_assert(sizeof(PointCloud2) <= kMaxPayloadBytes, "PointCloud2 exceeds one UDP datagram -- shrink kMaxPointFields/kMaxPointCloudBytes");
static_assert(sizeof(LaserScan) <= kMaxPayloadBytes, "LaserScan exceeds one UDP datagram -- shrink kMaxLaserScanPoints");
static_assert(sizeof(JointState) <= kMaxPayloadBytes, "JointState exceeds one UDP datagram -- shrink kMaxJoints");
static_assert(sizeof(Imu) <= kMaxPayloadBytes, "Imu exceeds one UDP datagram");
static_assert(sizeof(NavSatFix) <= kMaxPayloadBytes, "NavSatFix exceeds one UDP datagram");
static_assert(std::is_trivially_copyable<Temperature>::value, "Temperature must stay trivially copyable");
static_assert(std::is_trivially_copyable<Range>::value, "Range must stay trivially copyable");

} // namespace sensor_msgs

namespace nav_msgs {

#pragma pack(push, 1)

// nav_msgs/Odometry -- pose + twist, each with a 6x6 covariance matrix,
// plus a fixed-size child_frame_id (see file-level comment on strings).
struct Odometry {
    std_msgs::Header header;
    char child_frame_id[std_msgs::kFrameIdMaxLen];
    geometry_msgs::Pose pose;
    float pose_covariance[36];
    geometry_msgs::Twist twist;
    float twist_covariance[36];

    void set_child_frame_id(const char* name) {
        std::memset(child_frame_id, 0, sizeof(child_frame_id));
        std::strncpy(child_frame_id, name, sizeof(child_frame_id) - 1);
    }
    std::string child_frame_id_str() const {
        char buf[std_msgs::kFrameIdMaxLen + 1];
        std::memcpy(buf, child_frame_id, std_msgs::kFrameIdMaxLen);
        buf[std_msgs::kFrameIdMaxLen] = '\0';
        return std::string(buf);
    }
};

// nav_msgs/MapMetaData
struct MapMetaData {
    std_msgs::Time map_load_time;
    float resolution;
    uint32_t width;
    uint32_t height;
    geometry_msgs::Pose origin;
};

inline constexpr uint32_t kMaxPathPoses = 16;

// nav_msgs/Path -- ROS's `geometry_msgs/PoseStamped[] poses` becomes a
// fixed-capacity array (see geometry_msgs::PoseArray's comment).
struct Path {
    std_msgs::Header header;
    uint32_t count; // valid entries in poses[0..count), <= kMaxPathPoses
    geometry_msgs::PoseStamped poses[kMaxPathPoses];
};

// 1024 (a 32x32 grid), not a rounder 4096 -- 4096 doesn't fit ReLink's
// single-datagram budget (see the static_assert below); a real-size
// map needs tiling into multiple messages, see the struct comment.
inline constexpr uint32_t kMaxOccupancyGridCells = 1024;

// nav_msgs/OccupancyGrid -- ROS's `int8[] data` (one entry per cell,
// typically far more than kMaxOccupancyGridCells for a real map) is
// capped the same way PointCloud2's data is; for a real-size map, tile
// it into multiple OccupancyGrid messages or use a dedicated
// chunked-transfer path the way Image does.
struct OccupancyGrid {
    std_msgs::Header header;
    MapMetaData info;
    uint32_t data_len; // valid cells in data[0..data_len), <= kMaxOccupancyGridCells
    int8_t data[kMaxOccupancyGridCells];
};

inline constexpr uint32_t kMaxGridCells = 64;

// nav_msgs/GridCells
struct GridCells {
    std_msgs::Header header;
    float cell_width;
    float cell_height;
    uint32_t count; // valid entries in cells[0..count), <= kMaxGridCells
    geometry_msgs::Point cells[kMaxGridCells];
};

#pragma pack(pop)

static_assert(std::is_trivially_copyable<Odometry>::value, "Odometry must stay trivially copyable");
static_assert(std::is_trivially_copyable<MapMetaData>::value, "MapMetaData must stay trivially copyable");
static_assert(std::is_trivially_copyable<Path>::value, "Path must stay trivially copyable");
static_assert(std::is_trivially_copyable<OccupancyGrid>::value, "OccupancyGrid must stay trivially copyable");
static_assert(std::is_trivially_copyable<GridCells>::value, "GridCells must stay trivially copyable");

static_assert(sizeof(Odometry) <= kMaxPayloadBytes, "Odometry exceeds one UDP datagram");
static_assert(sizeof(Path) <= kMaxPayloadBytes, "Path exceeds one UDP datagram -- shrink kMaxPathPoses");
static_assert(sizeof(OccupancyGrid) <= kMaxPayloadBytes, "OccupancyGrid exceeds one UDP datagram -- shrink kMaxOccupancyGridCells");
static_assert(sizeof(GridCells) <= kMaxPayloadBytes, "GridCells exceeds one UDP datagram -- shrink kMaxGridCells");

} // namespace nav_msgs

namespace diagnostic_msgs {

#pragma pack(push, 1)

// 32, not a rounder 64 -- see kMaxKeyValues/kMaxDiagnosticStatuses below:
// DiagnosticArray nests DiagnosticStatus which nests KeyValue, and all
// three sizes compound fast against ReLink's single-datagram budget.
inline constexpr size_t kKvMaxLen = 32;

// diagnostic_msgs/KeyValue -- both fields fixed kKvMaxLen-byte strings
// (see the file-level comment on strings).
struct KeyValue {
    char key[kKvMaxLen];
    char value[kKvMaxLen];

    void set_key(const char* s) { std::memset(key, 0, sizeof(key)); std::strncpy(key, s, sizeof(key) - 1); }
    void set_value(const char* s) { std::memset(value, 0, sizeof(value)); std::strncpy(value, s, sizeof(value) - 1); }
    std::string key_str() const {
        char buf[kKvMaxLen + 1]; std::memcpy(buf, key, kKvMaxLen); buf[kKvMaxLen] = '\0'; return std::string(buf);
    }
    std::string value_str() const {
        char buf[kKvMaxLen + 1]; std::memcpy(buf, value, kKvMaxLen); buf[kKvMaxLen] = '\0'; return std::string(buf);
    }
};

inline constexpr uint32_t kMaxKeyValues = 3;

// diagnostic_msgs/DiagnosticStatus
struct DiagnosticStatus {
    static constexpr int8_t kOk = 0, kWarn = 1, kError = 2, kStale = 3;
    int8_t level;
    char name[std_msgs::kStringMaxLen];
    char message[std_msgs::kStringMaxLen];
    char hardware_id[std_msgs::kStringMaxLen];
    uint32_t values_count; // valid entries in values[0..values_count), <= kMaxKeyValues
    KeyValue values[kMaxKeyValues];

    void set_name(const char* s) { std::memset(name, 0, sizeof(name)); std::strncpy(name, s, sizeof(name) - 1); }
    void set_message(const char* s) { std::memset(message, 0, sizeof(message)); std::strncpy(message, s, sizeof(message) - 1); }
    void set_hardware_id(const char* s) { std::memset(hardware_id, 0, sizeof(hardware_id)); std::strncpy(hardware_id, s, sizeof(hardware_id) - 1); }
};

inline constexpr uint32_t kMaxDiagnosticStatuses = 3;

// diagnostic_msgs/DiagnosticArray
struct DiagnosticArray {
    std_msgs::Header header;
    uint32_t status_count; // valid entries in status[0..status_count), <= kMaxDiagnosticStatuses
    DiagnosticStatus status[kMaxDiagnosticStatuses];
};

#pragma pack(pop)

static_assert(std::is_trivially_copyable<KeyValue>::value, "KeyValue must stay trivially copyable");
static_assert(std::is_trivially_copyable<DiagnosticStatus>::value, "DiagnosticStatus must stay trivially copyable");
static_assert(std::is_trivially_copyable<DiagnosticArray>::value, "DiagnosticArray must stay trivially copyable");

static_assert(sizeof(DiagnosticArray) <= kMaxPayloadBytes,
              "DiagnosticArray exceeds one UDP datagram -- shrink kKvMaxLen/kMaxKeyValues/kMaxDiagnosticStatuses");

} // namespace diagnostic_msgs

namespace trajectory_msgs {

#pragma pack(push, 1)

// 4, not a rounder 8 -- JointTrajectory nests kMaxTrajectoryPoints many
// JointTrajectoryPoints, and MultiDOFJointTrajectory nests
// kMaxMultiDOFTrajectoryPoints many (larger) MultiDOFJointTrajectoryPoints;
// both compound fast against ReLink's single-datagram budget (see the
// static_asserts below).
inline constexpr uint32_t kMaxDOF = 4; // degrees of freedom per trajectory point

// trajectory_msgs/JointTrajectoryPoint -- ROS's `float64[]` arrays
// become fixed kMaxDOF-capacity arrays (see geometry_msgs::PoseArray's
// comment); all four share one `count`, same rationale as
// sensor_msgs::JointState.
struct JointTrajectoryPoint {
    uint32_t count; // valid entries in every array below, <= kMaxDOF
    double positions[kMaxDOF];
    double velocities[kMaxDOF];
    double accelerations[kMaxDOF];
    double effort[kMaxDOF];
    std_msgs::Duration time_from_start;
};

inline constexpr uint32_t kMaxTrajectoryPoints = 6;

// trajectory_msgs/JointTrajectory
struct JointTrajectory {
    std_msgs::Header header;
    uint32_t joint_names_count; // valid entries, <= kMaxDOF
    char joint_names[kMaxDOF][std_msgs::kStringMaxLen];
    uint32_t points_count; // valid entries in points[0..points_count), <= kMaxTrajectoryPoints
    JointTrajectoryPoint points[kMaxTrajectoryPoints];

    void set_joint_name(uint32_t i, const char* s) {
        std::memset(joint_names[i], 0, std_msgs::kStringMaxLen);
        std::strncpy(joint_names[i], s, std_msgs::kStringMaxLen - 1);
    }
};

// trajectory_msgs/MultiDOFJointTrajectoryPoint
struct MultiDOFJointTrajectoryPoint {
    uint32_t count; // valid entries in every array below, <= kMaxDOF
    geometry_msgs::Transform transforms[kMaxDOF];
    geometry_msgs::Twist velocities[kMaxDOF];
    geometry_msgs::Twist accelerations[kMaxDOF];
    std_msgs::Duration time_from_start;
};

// Smaller than kMaxTrajectoryPoints since MultiDOFJointTrajectoryPoint
// (transforms+twists per DOF) is larger than JointTrajectoryPoint
// (plain doubles per DOF) -- see the static_assert below.
inline constexpr uint32_t kMaxMultiDOFTrajectoryPoints = 3;

// trajectory_msgs/MultiDOFJointTrajectory
struct MultiDOFJointTrajectory {
    std_msgs::Header header;
    uint32_t joint_names_count;
    char joint_names[kMaxDOF][std_msgs::kStringMaxLen];
    uint32_t points_count; // <= kMaxMultiDOFTrajectoryPoints
    MultiDOFJointTrajectoryPoint points[kMaxMultiDOFTrajectoryPoints];

    void set_joint_name(uint32_t i, const char* s) {
        std::memset(joint_names[i], 0, std_msgs::kStringMaxLen);
        std::strncpy(joint_names[i], s, std_msgs::kStringMaxLen - 1);
    }
};

#pragma pack(pop)

static_assert(std::is_trivially_copyable<JointTrajectoryPoint>::value, "JointTrajectoryPoint must stay trivially copyable");
static_assert(std::is_trivially_copyable<JointTrajectory>::value, "JointTrajectory must stay trivially copyable");
static_assert(std::is_trivially_copyable<MultiDOFJointTrajectoryPoint>::value, "MultiDOFJointTrajectoryPoint must stay trivially copyable");
static_assert(std::is_trivially_copyable<MultiDOFJointTrajectory>::value, "MultiDOFJointTrajectory must stay trivially copyable");

static_assert(sizeof(JointTrajectory) <= kMaxPayloadBytes,
              "JointTrajectory exceeds one UDP datagram -- shrink kMaxDOF/kMaxTrajectoryPoints");
static_assert(sizeof(MultiDOFJointTrajectory) <= kMaxPayloadBytes,
              "MultiDOFJointTrajectory exceeds one UDP datagram -- shrink kMaxDOF/kMaxMultiDOFTrajectoryPoints");

} // namespace trajectory_msgs

namespace actionlib_msgs {

#pragma pack(push, 1)

// actionlib_msgs/GoalID -- ROS's `string id` becomes a fixed
// kStringMaxLen-byte buffer (see the file-level comment on strings).
struct GoalID {
    std_msgs::Time stamp;
    char id[std_msgs::kStringMaxLen];

    void set_id(const char* s) { std::memset(id, 0, sizeof(id)); std::strncpy(id, s, sizeof(id) - 1); }
    std::string id_str() const {
        char buf[std_msgs::kStringMaxLen + 1]; std::memcpy(buf, id, std_msgs::kStringMaxLen);
        buf[std_msgs::kStringMaxLen] = '\0'; return std::string(buf);
    }
};

// actionlib_msgs/GoalStatus
struct GoalStatus {
    static constexpr uint8_t kPending = 0, kActive = 1, kPreempted = 2, kSucceeded = 3, kAborted = 4,
                              kRejected = 5, kPreempting = 6, kRecalling = 7, kRecalled = 8, kLost = 9;
    GoalID goal_id;
    uint8_t status;
    char text[std_msgs::kStringMaxLen];

    void set_text(const char* s) { std::memset(text, 0, sizeof(text)); std::strncpy(text, s, sizeof(text) - 1); }
    std::string text_str() const {
        char buf[std_msgs::kStringMaxLen + 1]; std::memcpy(buf, text, std_msgs::kStringMaxLen);
        buf[std_msgs::kStringMaxLen] = '\0'; return std::string(buf);
    }
};

inline constexpr uint32_t kMaxGoalStatuses = 8;

// actionlib_msgs/GoalStatusArray -- ROS's `GoalStatus[] status_list`
// becomes a fixed kMaxGoalStatuses-capacity array (see
// geometry_msgs::PoseArray's comment on the general pattern).
struct GoalStatusArray {
    std_msgs::Header header;
    uint32_t status_list_count; // valid entries, <= kMaxGoalStatuses
    GoalStatus status_list[kMaxGoalStatuses];
};

#pragma pack(pop)

static_assert(std::is_trivially_copyable<GoalID>::value, "GoalID must stay trivially copyable");
static_assert(std::is_trivially_copyable<GoalStatus>::value, "GoalStatus must stay trivially copyable");
static_assert(std::is_trivially_copyable<GoalStatusArray>::value, "GoalStatusArray must stay trivially copyable");

static_assert(sizeof(GoalStatusArray) <= kMaxPayloadBytes,
              "GoalStatusArray exceeds one UDP datagram -- shrink kMaxGoalStatuses");

} // namespace actionlib_msgs

} // namespace relink

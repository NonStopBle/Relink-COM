#!/usr/bin/env python3
"""standard_msgs_pubsub -- publishes and subscribes every ROS-familiar
composite type ReLink ships (relink/standard_msgs.py, what the file
itself calls "NoROSLib" naming), one topic per type, in a single
runnable file. Wire-compatible with cpp/examples/standard_msgs_pubsub.cpp
-- run one copy of each and they talk to each other with zero changes.

Types covered (49 total, everything in standard_msgs.py except
sensor_msgs.ImageChunk, which needs advertise_image/publish_image/
subscribe_image instead of plain advertise/publish/subscribe -- see
the README's "Sending images" step and camera_stream.py):

    std_msgs:        Empty, Time, Duration, ColorRGBA, Header, String
    geometry_msgs:   Vector3, Point, Point32, Quaternion, Pose, Twist,
                      Accel, Wrench, PoseStamped, TwistStamped,
                      Transform, TransformStamped, PoseWithCovariance,
                      TwistWithCovariance, PoseArray, Polygon
    sensor_msgs:     Imu, NavSatStatus, NavSatFix, MagneticField,
                      Temperature, Range, RegionOfInterest, CameraInfo,
                      PointField, PointCloud2, LaserScan, JointState
    nav_msgs:        Odometry, MapMetaData, Path, OccupancyGrid, GridCells
    diagnostic_msgs: KeyValue, DiagnosticStatus, DiagnosticArray
    trajectory_msgs: JointTrajectoryPoint, JointTrajectory,
                      MultiDOFJointTrajectoryPoint, MultiDOFJointTrajectory
    actionlib_msgs:  GoalID, GoalStatus, GoalStatusArray

Run (in two terminals, or on two machines):
    python3 standard_msgs_pubsub.py
"""
import sys
import os
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from relink import RelinkNode
from relink import std_msgs, geometry_msgs, sensor_msgs, nav_msgs
from relink import diagnostic_msgs, trajectory_msgs, actionlib_msgs


def main():
    node = RelinkNode()
    node.use_multicast_discovery()   # zero setup -- see Step 3

    # -----------------------------------------------------------
    # Subscribe to every topic first, so an early message from a
    # peer that started first is never missed.
    # -----------------------------------------------------------

    # --- std_msgs ---
    node.subscribe("/relink/std/empty", std_msgs.Empty, lambda m: print("std_msgs/Empty received"))
    node.subscribe("/relink/std/time", std_msgs.Time, lambda m: print(f"std_msgs/Time     sec={m.sec} nsec={m.nsec}"))
    node.subscribe("/relink/std/duration", std_msgs.Duration, lambda m: print(f"std_msgs/Duration sec={m.sec} nsec={m.nsec}"))
    node.subscribe("/relink/std/color", std_msgs.ColorRGBA, lambda m: print(f"std_msgs/ColorRGBA r={m.r:.2f} g={m.g:.2f} b={m.b:.2f} a={m.a:.2f}"))
    node.subscribe("/relink/std/header", std_msgs.Header, lambda m: print(f"std_msgs/Header   seq={m.seq} frame={m.frame_id_str()}"))
    node.subscribe("/relink/std/string", std_msgs.String, lambda m: print(f"std_msgs/String   \"{m.str()}\""))

    # --- geometry_msgs ---
    node.subscribe("/relink/geo/vector3", geometry_msgs.Vector3, lambda m: print(f"geometry_msgs/Vector3 ({m.x:.2f}, {m.y:.2f}, {m.z:.2f})"))
    node.subscribe("/relink/geo/point", geometry_msgs.Point, lambda m: print(f"geometry_msgs/Point   ({m.x:.2f}, {m.y:.2f}, {m.z:.2f})"))
    node.subscribe("/relink/geo/point32", geometry_msgs.Point32, lambda m: print(f"geometry_msgs/Point32 ({m.x:.2f}, {m.y:.2f}, {m.z:.2f})"))
    node.subscribe("/relink/geo/quaternion", geometry_msgs.Quaternion, lambda m: print(f"geometry_msgs/Quaternion ({m.x:.2f}, {m.y:.2f}, {m.z:.2f}, {m.w:.2f})"))
    node.subscribe("/relink/geo/pose", geometry_msgs.Pose, lambda m: print(f"geometry_msgs/Pose    pos=({m.position.x:.2f}, {m.position.y:.2f}, {m.position.z:.2f})"))
    node.subscribe("/relink/geo/twist", geometry_msgs.Twist, lambda m: print(f"geometry_msgs/Twist   linear.x={m.linear.x:.2f} angular.z={m.angular.z:.2f}"))
    node.subscribe("/relink/geo/accel", geometry_msgs.Accel, lambda m: print(f"geometry_msgs/Accel   linear.x={m.linear.x:.2f}"))
    node.subscribe("/relink/geo/wrench", geometry_msgs.Wrench, lambda m: print(f"geometry_msgs/Wrench  force.z={m.force.z:.2f}"))
    node.subscribe("/relink/geo/pose_stamped", geometry_msgs.PoseStamped, lambda m: print(f"geometry_msgs/PoseStamped frame={m.header.frame_id_str()}"))
    node.subscribe("/relink/geo/twist_stamped", geometry_msgs.TwistStamped, lambda m: print(f"geometry_msgs/TwistStamped frame={m.header.frame_id_str()}"))
    node.subscribe("/relink/geo/transform", geometry_msgs.Transform, lambda m: print(f"geometry_msgs/Transform translation.x={m.translation.x:.2f}"))
    node.subscribe("/relink/geo/transform_stamped", geometry_msgs.TransformStamped, lambda m: print(f"geometry_msgs/TransformStamped child={m.child_frame_id_str()}"))
    node.subscribe("/relink/geo/pose_cov", geometry_msgs.PoseWithCovariance, lambda m: print(f"geometry_msgs/PoseWithCovariance cov[0]={m.covariance[0]:.2f}"))
    node.subscribe("/relink/geo/twist_cov", geometry_msgs.TwistWithCovariance, lambda m: print(f"geometry_msgs/TwistWithCovariance cov[0]={m.covariance[0]:.2f}"))
    node.subscribe("/relink/geo/pose_array", geometry_msgs.PoseArray, lambda m: print(f"geometry_msgs/PoseArray count={m.count}"))
    node.subscribe("/relink/geo/polygon", geometry_msgs.Polygon, lambda m: print(f"geometry_msgs/Polygon count={m.count}"))

    # --- sensor_msgs ---
    node.subscribe("/relink/sensor/imu", sensor_msgs.Imu, lambda m: print(f"sensor_msgs/Imu       accel.z={m.linear_acceleration.z:.2f}"))
    node.subscribe("/relink/sensor/navsat_status", sensor_msgs.NavSatStatus, lambda m: print(f"sensor_msgs/NavSatStatus status={m.status}"))
    node.subscribe("/relink/sensor/navsat_fix", sensor_msgs.NavSatFix, lambda m: print(f"sensor_msgs/NavSatFix lat={m.latitude:.6f} lon={m.longitude:.6f}"))
    node.subscribe("/relink/sensor/mag", sensor_msgs.MagneticField, lambda m: print(f"sensor_msgs/MagneticField x={m.magnetic_field.x:.2f}"))
    node.subscribe("/relink/sensor/temp", sensor_msgs.Temperature, lambda m: print(f"sensor_msgs/Temperature {m.temperature:.1f} C"))
    node.subscribe("/relink/sensor/range", sensor_msgs.Range, lambda m: print(f"sensor_msgs/Range     {m.range:.2f} m"))
    node.subscribe("/relink/sensor/roi", sensor_msgs.RegionOfInterest, lambda m: print(f"sensor_msgs/RegionOfInterest {m.width}x{m.height}"))
    node.subscribe("/relink/sensor/camera_info", sensor_msgs.CameraInfo, lambda m: print(f"sensor_msgs/CameraInfo {m.width}x{m.height} model={m.distortion_model_str()}"))
    node.subscribe("/relink/sensor/point_field", sensor_msgs.PointField, lambda m: print(f"sensor_msgs/PointField name={m.name_str()}"))
    node.subscribe("/relink/sensor/point_cloud2", sensor_msgs.PointCloud2, lambda m: print(f"sensor_msgs/PointCloud2 {m.width}x{m.height} fields={m.fields_count}"))
    node.subscribe("/relink/sensor/laser_scan", sensor_msgs.LaserScan, lambda m: print(f"sensor_msgs/LaserScan ranges_count={m.ranges_count}"))
    node.subscribe("/relink/sensor/joint_state", sensor_msgs.JointState, lambda m: print(f"sensor_msgs/JointState count={m.count} name0={m.name_str(0)}"))

    # --- nav_msgs ---
    node.subscribe("/relink/nav/odometry", nav_msgs.Odometry, lambda m: print(f"nav_msgs/Odometry child={m.child_frame_id_str()}"))
    node.subscribe("/relink/nav/map_meta", nav_msgs.MapMetaData, lambda m: print(f"nav_msgs/MapMetaData {m.width}x{m.height} res={m.resolution:.2f}"))
    node.subscribe("/relink/nav/path", nav_msgs.Path, lambda m: print(f"nav_msgs/Path count={m.count}"))
    node.subscribe("/relink/nav/occupancy_grid", nav_msgs.OccupancyGrid, lambda m: print(f"nav_msgs/OccupancyGrid data_len={m.data_len}"))
    node.subscribe("/relink/nav/grid_cells", nav_msgs.GridCells, lambda m: print(f"nav_msgs/GridCells count={m.count}"))

    # --- diagnostic_msgs ---
    node.subscribe("/relink/diag/key_value", diagnostic_msgs.KeyValue, lambda m: print(f"diagnostic_msgs/KeyValue {m.key_str()}={m.value_str()}"))
    node.subscribe("/relink/diag/status", diagnostic_msgs.DiagnosticStatus, lambda m: print(f"diagnostic_msgs/DiagnosticStatus level={m.level} name={m.name_str()}"))
    node.subscribe("/relink/diag/array", diagnostic_msgs.DiagnosticArray, lambda m: print(f"diagnostic_msgs/DiagnosticArray status_count={m.status_count}"))

    # --- trajectory_msgs ---
    node.subscribe("/relink/traj/point", trajectory_msgs.JointTrajectoryPoint, lambda m: print(f"trajectory_msgs/JointTrajectoryPoint count={m.count} pos0={m.positions[0]:.2f}"))
    node.subscribe("/relink/traj/trajectory", trajectory_msgs.JointTrajectory, lambda m: print(f"trajectory_msgs/JointTrajectory points_count={m.points_count}"))
    node.subscribe("/relink/traj/multidof_point", trajectory_msgs.MultiDOFJointTrajectoryPoint, lambda m: print(f"trajectory_msgs/MultiDOFJointTrajectoryPoint count={m.count}"))
    node.subscribe("/relink/traj/multidof_trajectory", trajectory_msgs.MultiDOFJointTrajectory, lambda m: print(f"trajectory_msgs/MultiDOFJointTrajectory points_count={m.points_count}"))

    # --- actionlib_msgs ---
    node.subscribe("/relink/action/goal_id", actionlib_msgs.GoalID, lambda m: print(f"actionlib_msgs/GoalID id={m.id_str()}"))
    node.subscribe("/relink/action/goal_status", actionlib_msgs.GoalStatus, lambda m: print(f"actionlib_msgs/GoalStatus status={m.status} text={m.text_str()}"))
    node.subscribe("/relink/action/goal_status_array", actionlib_msgs.GoalStatusArray, lambda m: print(f"actionlib_msgs/GoalStatusArray status_list_count={m.status_list_count}"))

    # -----------------------------------------------------------
    # Advertise every topic (same list, same order).
    # -----------------------------------------------------------
    node.advertise("/relink/std/empty", std_msgs.Empty)
    node.advertise("/relink/std/time", std_msgs.Time)
    node.advertise("/relink/std/duration", std_msgs.Duration)
    node.advertise("/relink/std/color", std_msgs.ColorRGBA)
    node.advertise("/relink/std/header", std_msgs.Header)
    node.advertise("/relink/std/string", std_msgs.String)

    node.advertise("/relink/geo/vector3", geometry_msgs.Vector3)
    node.advertise("/relink/geo/point", geometry_msgs.Point)
    node.advertise("/relink/geo/point32", geometry_msgs.Point32)
    node.advertise("/relink/geo/quaternion", geometry_msgs.Quaternion)
    node.advertise("/relink/geo/pose", geometry_msgs.Pose)
    node.advertise("/relink/geo/twist", geometry_msgs.Twist)
    node.advertise("/relink/geo/accel", geometry_msgs.Accel)
    node.advertise("/relink/geo/wrench", geometry_msgs.Wrench)
    node.advertise("/relink/geo/pose_stamped", geometry_msgs.PoseStamped)
    node.advertise("/relink/geo/twist_stamped", geometry_msgs.TwistStamped)
    node.advertise("/relink/geo/transform", geometry_msgs.Transform)
    node.advertise("/relink/geo/transform_stamped", geometry_msgs.TransformStamped)
    node.advertise("/relink/geo/pose_cov", geometry_msgs.PoseWithCovariance)
    node.advertise("/relink/geo/twist_cov", geometry_msgs.TwistWithCovariance)
    node.advertise("/relink/geo/pose_array", geometry_msgs.PoseArray)
    node.advertise("/relink/geo/polygon", geometry_msgs.Polygon)

    node.advertise("/relink/sensor/imu", sensor_msgs.Imu)
    node.advertise("/relink/sensor/navsat_status", sensor_msgs.NavSatStatus)
    node.advertise("/relink/sensor/navsat_fix", sensor_msgs.NavSatFix)
    node.advertise("/relink/sensor/mag", sensor_msgs.MagneticField)
    node.advertise("/relink/sensor/temp", sensor_msgs.Temperature)
    node.advertise("/relink/sensor/range", sensor_msgs.Range)
    node.advertise("/relink/sensor/roi", sensor_msgs.RegionOfInterest)
    node.advertise("/relink/sensor/camera_info", sensor_msgs.CameraInfo)
    node.advertise("/relink/sensor/point_field", sensor_msgs.PointField)
    node.advertise("/relink/sensor/point_cloud2", sensor_msgs.PointCloud2)
    node.advertise("/relink/sensor/laser_scan", sensor_msgs.LaserScan)
    node.advertise("/relink/sensor/joint_state", sensor_msgs.JointState)

    node.advertise("/relink/nav/odometry", nav_msgs.Odometry)
    node.advertise("/relink/nav/map_meta", nav_msgs.MapMetaData)
    node.advertise("/relink/nav/path", nav_msgs.Path)
    node.advertise("/relink/nav/occupancy_grid", nav_msgs.OccupancyGrid)
    node.advertise("/relink/nav/grid_cells", nav_msgs.GridCells)

    node.advertise("/relink/diag/key_value", diagnostic_msgs.KeyValue)
    node.advertise("/relink/diag/status", diagnostic_msgs.DiagnosticStatus)
    node.advertise("/relink/diag/array", diagnostic_msgs.DiagnosticArray)

    node.advertise("/relink/traj/point", trajectory_msgs.JointTrajectoryPoint)
    node.advertise("/relink/traj/trajectory", trajectory_msgs.JointTrajectory)
    node.advertise("/relink/traj/multidof_point", trajectory_msgs.MultiDOFJointTrajectoryPoint)
    node.advertise("/relink/traj/multidof_trajectory", trajectory_msgs.MultiDOFJointTrajectory)

    node.advertise("/relink/action/goal_id", actionlib_msgs.GoalID)
    node.advertise("/relink/action/goal_status", actionlib_msgs.GoalStatus)
    node.advertise("/relink/action/goal_status_array", actionlib_msgs.GoalStatusArray)

    i = 0
    while True:
        node.spin_once()   # services discovery -- call this every loop
        t = i * 0.1

        # --- std_msgs ---
        node.publish("/relink/std/empty", std_msgs.Empty())
        node.publish("/relink/std/time", std_msgs.Time.now())
        node.publish("/relink/std/duration", std_msgs.Duration(sec=i, nsec=0))
        node.publish("/relink/std/color", std_msgs.ColorRGBA(r=1.0, g=0.5, b=0.0, a=1.0))

        h = std_msgs.Header()
        h.seq = i
        h.stamp_now()
        h.set_frame_id("base_link")
        node.publish("/relink/std/header", h)

        s = std_msgs.String()
        s.set("hello from standard_msgs_pubsub")
        node.publish("/relink/std/string", s)

        # --- geometry_msgs ---
        node.publish("/relink/geo/vector3", geometry_msgs.Vector3(x=t, y=0, z=0))
        node.publish("/relink/geo/point", geometry_msgs.Point(x=t, y=0, z=0))
        node.publish("/relink/geo/point32", geometry_msgs.Point32(x=t, y=0, z=0))
        node.publish("/relink/geo/quaternion", geometry_msgs.Quaternion(x=0, y=0, z=0, w=1))

        pose = geometry_msgs.Pose()
        pose.position = geometry_msgs.Point(x=t, y=0, z=0)
        pose.orientation = geometry_msgs.Quaternion(x=0, y=0, z=0, w=1)
        node.publish("/relink/geo/pose", pose)

        twist = geometry_msgs.Twist()
        twist.linear = geometry_msgs.Vector3(x=t, y=0, z=0)
        twist.angular = geometry_msgs.Vector3(x=0, y=0, z=0.1)
        node.publish("/relink/geo/twist", twist)

        accel = geometry_msgs.Accel()
        accel.linear = geometry_msgs.Vector3(x=t, y=0, z=0)
        node.publish("/relink/geo/accel", accel)

        wrench = geometry_msgs.Wrench()
        wrench.force = geometry_msgs.Vector3(x=0, y=0, z=t)
        node.publish("/relink/geo/wrench", wrench)

        ps = geometry_msgs.PoseStamped()
        ps.header.set_frame_id("map")
        ps.pose.position = geometry_msgs.Point(x=t, y=0, z=0)
        node.publish("/relink/geo/pose_stamped", ps)

        ts = geometry_msgs.TwistStamped()
        ts.header.set_frame_id("map")
        node.publish("/relink/geo/twist_stamped", ts)

        transform = geometry_msgs.Transform()
        transform.translation = geometry_msgs.Vector3(x=t, y=0, z=0)
        transform.rotation = geometry_msgs.Quaternion(x=0, y=0, z=0, w=1)
        node.publish("/relink/geo/transform", transform)

        tfs = geometry_msgs.TransformStamped()
        tfs.set_child_frame_id("base_link")
        node.publish("/relink/geo/transform_stamped", tfs)

        pc = geometry_msgs.PoseWithCovariance()
        pc.pose.position = geometry_msgs.Point(x=t, y=0, z=0)
        node.publish("/relink/geo/pose_cov", pc)

        tc = geometry_msgs.TwistWithCovariance()
        tc.twist.linear = geometry_msgs.Vector3(x=t, y=0, z=0)
        node.publish("/relink/geo/twist_cov", tc)

        pa = geometry_msgs.PoseArray()
        pa.count = 2
        pa.poses[0].position = geometry_msgs.Point(x=0, y=0, z=0)
        pa.poses[1].position = geometry_msgs.Point(x=t, y=0, z=0)
        node.publish("/relink/geo/pose_array", pa)

        poly = geometry_msgs.Polygon()
        poly.count = 3
        poly.points[0] = geometry_msgs.Point32(x=0, y=0, z=0)
        poly.points[1] = geometry_msgs.Point32(x=1, y=0, z=0)
        poly.points[2] = geometry_msgs.Point32(x=0, y=1, z=0)
        node.publish("/relink/geo/polygon", poly)

        # --- sensor_msgs ---
        imu = sensor_msgs.Imu()
        imu.header.set_frame_id("imu_link")
        imu.orientation = geometry_msgs.Quaternion(x=0, y=0, z=0, w=1)
        imu.linear_acceleration = geometry_msgs.Vector3(x=0, y=0, z=9.81)
        node.publish("/relink/sensor/imu", imu)

        node.publish("/relink/sensor/navsat_status", sensor_msgs.NavSatStatus(
            status=sensor_msgs.NavSatStatus.FIX, service=sensor_msgs.NavSatStatus.SERVICE_GPS))

        fix = sensor_msgs.NavSatFix()
        fix.header.set_frame_id("gps")
        fix.status = sensor_msgs.NavSatStatus(status=sensor_msgs.NavSatStatus.FIX, service=sensor_msgs.NavSatStatus.SERVICE_GPS)
        fix.latitude = 13.7563 + i * 0.0001
        fix.longitude = 100.5018
        node.publish("/relink/sensor/navsat_fix", fix)

        mag = sensor_msgs.MagneticField()
        mag.header.set_frame_id("imu_link")
        mag.magnetic_field = geometry_msgs.Vector3(x=20.0, y=0.0, z=40.0)
        node.publish("/relink/sensor/mag", mag)

        temp = sensor_msgs.Temperature()
        temp.header.set_frame_id("cpu")
        temp.temperature = 36.6 + (i % 5)
        node.publish("/relink/sensor/temp", temp)

        rng = sensor_msgs.Range()
        rng.header.set_frame_id("sonar")
        rng.radiation_type = sensor_msgs.Range.ULTRASOUND
        rng.min_range = 0.02
        rng.max_range = 4.0
        rng.range = 1.0 + 0.01 * (i % 100)
        node.publish("/relink/sensor/range", rng)

        node.publish("/relink/sensor/roi", sensor_msgs.RegionOfInterest(
            x_offset=0, y_offset=0, height=480, width=640, do_rectify=1))

        cam = sensor_msgs.CameraInfo()
        cam.header.set_frame_id("camera")
        cam.width = 640
        cam.height = 480
        cam.set_distortion_model("plumb_bob")
        node.publish("/relink/sensor/camera_info", cam)

        pf = sensor_msgs.PointField()
        pf.set_name("x")
        pf.offset = 0
        pf.datatype = sensor_msgs.PointField.FLOAT32
        pf.count = 1
        node.publish("/relink/sensor/point_field", pf)

        cloud = sensor_msgs.PointCloud2()
        cloud.header.set_frame_id("lidar")
        cloud.width = 10
        cloud.height = 1
        cloud.fields_count = 1
        cloud.fields[0].set_name("x")
        node.publish("/relink/sensor/point_cloud2", cloud)

        scan = sensor_msgs.LaserScan()
        scan.header.set_frame_id("lidar")
        scan.angle_min = -1.57
        scan.angle_max = 1.57
        scan.ranges_count = 3
        scan.ranges[0], scan.ranges[1], scan.ranges[2] = 1.0, 2.0, 3.0
        node.publish("/relink/sensor/laser_scan", scan)

        js = sensor_msgs.JointState()
        js.header.set_frame_id("robot")
        js.count = 2
        js.set_name(0, "shoulder")
        js.set_name(1, "elbow")
        js.position[0] = 0.1 * i
        js.position[1] = 0.2 * i
        node.publish("/relink/sensor/joint_state", js)

        # --- nav_msgs ---
        odom = nav_msgs.Odometry()
        odom.header.set_frame_id("odom")
        odom.set_child_frame_id("base_link")
        odom.pose.position = geometry_msgs.Point(x=t, y=0, z=0)
        node.publish("/relink/nav/odometry", odom)

        meta = nav_msgs.MapMetaData()
        meta.map_load_time = std_msgs.Time.now()
        meta.resolution = 0.05
        meta.width = 100
        meta.height = 100
        node.publish("/relink/nav/map_meta", meta)

        path = nav_msgs.Path()
        path.header.set_frame_id("map")
        path.count = 2
        path.poses[0].pose.position = geometry_msgs.Point(x=0, y=0, z=0)
        path.poses[1].pose.position = geometry_msgs.Point(x=t, y=0, z=0)
        node.publish("/relink/nav/path", path)

        grid = nav_msgs.OccupancyGrid()
        grid.header.set_frame_id("map")
        grid.info.width = 10
        grid.info.height = 10
        grid.data_len = 100
        node.publish("/relink/nav/occupancy_grid", grid)

        cells = nav_msgs.GridCells()
        cells.header.set_frame_id("map")
        cells.cell_width = 0.1
        cells.cell_height = 0.1
        cells.count = 2
        cells.cells[0] = geometry_msgs.Point(x=0, y=0, z=0)
        cells.cells[1] = geometry_msgs.Point(x=t, y=0, z=0)
        node.publish("/relink/nav/grid_cells", cells)

        # --- diagnostic_msgs ---
        kv = diagnostic_msgs.KeyValue()
        kv.set_key("battery_pct")
        kv.set_value(str(100 - (i % 100)))
        node.publish("/relink/diag/key_value", kv)

        status = diagnostic_msgs.DiagnosticStatus()
        status.level = diagnostic_msgs.DiagnosticStatus.OK
        status.set_name("battery_monitor")
        status.set_message("nominal")
        status.set_hardware_id("bms_01")
        node.publish("/relink/diag/status", status)

        arr = diagnostic_msgs.DiagnosticArray()
        arr.header.set_frame_id("diagnostics")
        arr.status_count = 1
        arr.status[0].level = diagnostic_msgs.DiagnosticStatus.OK
        arr.status[0].set_name("battery_monitor")
        node.publish("/relink/diag/array", arr)

        # --- trajectory_msgs ---
        pt = trajectory_msgs.JointTrajectoryPoint()
        pt.count = 2
        pt.positions[0] = 0.1 * i
        pt.positions[1] = 0.2 * i
        node.publish("/relink/traj/point", pt)

        tj = trajectory_msgs.JointTrajectory()
        tj.header.set_frame_id("robot")
        tj.joint_names_count = 2
        tj.set_joint_name(0, "shoulder")
        tj.set_joint_name(1, "elbow")
        tj.points_count = 1
        tj.points[0].count = 2
        node.publish("/relink/traj/trajectory", tj)

        mdp = trajectory_msgs.MultiDOFJointTrajectoryPoint()
        mdp.count = 1
        mdp.transforms[0].translation = geometry_msgs.Vector3(x=t, y=0, z=0)
        node.publish("/relink/traj/multidof_point", mdp)

        mdt = trajectory_msgs.MultiDOFJointTrajectory()
        mdt.header.set_frame_id("robot")
        mdt.joint_names_count = 1
        mdt.set_joint_name(0, "base")
        mdt.points_count = 1
        node.publish("/relink/traj/multidof_trajectory", mdt)

        # --- actionlib_msgs ---
        gid = actionlib_msgs.GoalID()
        gid.stamp = std_msgs.Time.now()
        gid.set_id(f"goal_{i}")
        node.publish("/relink/action/goal_id", gid)

        gs = actionlib_msgs.GoalStatus()
        gs.status = actionlib_msgs.GoalStatus.ACTIVE
        gs.set_text("moving to goal")
        node.publish("/relink/action/goal_status", gs)

        gsa = actionlib_msgs.GoalStatusArray()
        gsa.header.set_frame_id("action_server")
        gsa.status_list_count = 1
        gsa.status_list[0].status = actionlib_msgs.GoalStatus.ACTIVE
        node.publish("/relink/action/goal_status_array", gsa)

        time.sleep(0.5)
        i += 1


if __name__ == "__main__":
    main()

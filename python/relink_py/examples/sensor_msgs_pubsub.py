#!/usr/bin/env python3
"""sensor_msgs_pubsub -- publishes and subscribes every sensor_msgs
type ReLink ships via plain advertise/publish/subscribe (relink/
standard_msgs.py): Imu, NavSatStatus, NavSatFix, MagneticField,
Temperature, Range, RegionOfInterest, CameraInfo, PointField,
PointCloud2, LaserScan, JointState. Wire-compatible with
cpp/examples/sensor_msgs_pubsub.cpp.

NOT covered here: sensor_msgs.Image and CompressedImage. Those map to
ReLink's ImageChunk (image.py), a chunked large-blob type that goes
through advertise_image/publish_image/subscribe_image instead -- see
camera_stream.py and the README's "Sending images" step.

Run (in two terminals, or on two machines):
    python3 sensor_msgs_pubsub.py
"""
import sys
import os
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from relink import RelinkNode
from relink import geometry_msgs, sensor_msgs


def main():
    node = RelinkNode()
    node.use_multicast_discovery()   # zero setup -- see Step 3

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

    i = 0
    while True:
        node.spin_once()   # services discovery -- call this every loop

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

        time.sleep(0.5)
        i += 1


if __name__ == "__main__":
    main()

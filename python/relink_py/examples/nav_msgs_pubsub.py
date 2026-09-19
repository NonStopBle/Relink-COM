#!/usr/bin/env python3
"""nav_msgs_pubsub -- publishes and subscribes every nav_msgs type
ReLink ships (relink/standard_msgs.py), one topic per type: Odometry,
MapMetaData, Path, OccupancyGrid, GridCells. Wire-compatible with
cpp/examples/nav_msgs_pubsub.cpp.

Run (in two terminals, or on two machines):
    python3 nav_msgs_pubsub.py
"""
import sys
import os
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from relink import RelinkNode
from relink import std_msgs, geometry_msgs, nav_msgs


def main():
    node = RelinkNode()
    node.use_multicast_discovery()   # zero setup -- see Step 3

    node.subscribe("/relink/nav/odometry", nav_msgs.Odometry, lambda m: print(f"nav_msgs/Odometry child={m.child_frame_id_str()}"))
    node.subscribe("/relink/nav/map_meta", nav_msgs.MapMetaData, lambda m: print(f"nav_msgs/MapMetaData {m.width}x{m.height} res={m.resolution:.2f}"))
    node.subscribe("/relink/nav/path", nav_msgs.Path, lambda m: print(f"nav_msgs/Path count={m.count}"))
    node.subscribe("/relink/nav/occupancy_grid", nav_msgs.OccupancyGrid, lambda m: print(f"nav_msgs/OccupancyGrid data_len={m.data_len}"))
    node.subscribe("/relink/nav/grid_cells", nav_msgs.GridCells, lambda m: print(f"nav_msgs/GridCells count={m.count}"))

    node.advertise("/relink/nav/odometry", nav_msgs.Odometry)
    node.advertise("/relink/nav/map_meta", nav_msgs.MapMetaData)
    node.advertise("/relink/nav/path", nav_msgs.Path)
    node.advertise("/relink/nav/occupancy_grid", nav_msgs.OccupancyGrid)
    node.advertise("/relink/nav/grid_cells", nav_msgs.GridCells)

    i = 0
    while True:
        node.spin_once()   # services discovery -- call this every loop
        t = i * 0.1

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

        time.sleep(0.5)
        i += 1


if __name__ == "__main__":
    main()

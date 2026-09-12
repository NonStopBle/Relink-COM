"""ROS-familiar import path: `from relink import nav_msgs; nav_msgs.Odometry`.
Real definition lives in standard_msgs.py."""
from .standard_msgs import (
    Odometry, MapMetaData, Path, OccupancyGrid, GridCells,
    MAX_PATH_POSES, MAX_OCCUPANCY_GRID_CELLS, MAX_GRID_CELLS,
)

__all__ = ["Odometry", "MapMetaData", "Path", "OccupancyGrid", "GridCells",
           "MAX_PATH_POSES", "MAX_OCCUPANCY_GRID_CELLS", "MAX_GRID_CELLS"]

"""ROS-familiar import path: `from relink import trajectory_msgs; trajectory_msgs.JointTrajectory`.
Real definitions live in standard_msgs.py."""
from .standard_msgs import (
    JointTrajectoryPoint, JointTrajectory, MultiDOFJointTrajectoryPoint, MultiDOFJointTrajectory,
    MAX_DOF, MAX_TRAJECTORY_POINTS, MAX_MULTI_DOF_TRAJECTORY_POINTS,
)

__all__ = ["JointTrajectoryPoint", "JointTrajectory",
           "MultiDOFJointTrajectoryPoint", "MultiDOFJointTrajectory",
           "MAX_DOF", "MAX_TRAJECTORY_POINTS", "MAX_MULTI_DOF_TRAJECTORY_POINTS"]

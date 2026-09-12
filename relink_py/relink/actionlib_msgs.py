"""ROS-familiar import path: `from relink import actionlib_msgs; actionlib_msgs.GoalStatus`.
Real definitions live in standard_msgs.py."""
from .standard_msgs import GoalID, GoalStatus, GoalStatusArray, MAX_GOAL_STATUSES

__all__ = ["GoalID", "GoalStatus", "GoalStatusArray", "MAX_GOAL_STATUSES"]

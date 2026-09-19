#!/usr/bin/env bash
# Smoke test for all four camera pub/sub pairs in this comparison suite
# (Python ReLink, Python ROS2, C++ ReLink, C++ ROS2). Runs each pair for
# a few seconds against a real rlcore, and fails loudly if a subscriber
# never reports a single received frame.
#
# Reads frames from a video file (--video), not a live camera, by
# default -- a real /dev/video0 only allows one exclusive open at a
# time and this test runs 4 publishers back to back, which was
# intermittently flaky against a live webcam (a cold V4L2 open
# sometimes returns isOpened()==true but reads fail silently for a
# beat, busy-spinning with nothing printed). A file has none of that.
#
# Requires: a running rlcore (`python3 rlcore/relink_rlcore.py`, from
# python/), OpenCV, and (for the ROS2 pairs) ROS2 sourced.
#
# Run: ./smoke_test.sh [rlcore_ip] [video_path]
#   defaults: rlcore_ip=127.0.0.1
#             video_path=/home/thinkpad/proj/Remind/ReVision/tracker/video_many_car.mp4

set -u
RLCORE_IP="${1:-127.0.0.1}"
VIDEO="${2:-/home/thinkpad/proj/Remind/ReVision/tracker/video_many_car.mp4}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CPP_DIR="$SCRIPT_DIR/../../../../cpp"
DURATION=6
FAIL=0

if [ ! -f "$VIDEO" ]; then
    echo "FAIL: video file not found: $VIDEO"
    exit 1
fi

has_frames() {
    grep -q "frames," "$1" 2>/dev/null
}

# Pre-warm cv2's shared-library load (and rclpy's, if available) --
# the very first cv2/rclpy import in a fresh shell measurably outpaces
# the fixed pub/sub startup window below (cold dynamic-library load
# from disk vs. warm page cache), which was making only the very FIRST
# pair below flaky while every later one passed reliably. Paying that
# cost once, up front, outside the timed windows, fixes it.
echo "(pre-warming cv2/rclpy imports...)"
python3 -c "import cv2, numpy" 2>/dev/null
python3 -c "import rclpy, sensor_msgs.msg" 2>/dev/null

# Runs one pub/sub pair once. Backgrounded pub/sub startup (process
# spawn, imports, discovery handshake) has occasionally raced past a
# single attempt's window under heavy concurrent process churn on this
# machine (four pairs launched back to back) even though the exact same
# command reliably works in isolation -- so a single failed attempt
# retries once (see run_pair) before this is treated as a real failure.
try_once() {
    local pub_cmd="$1" sub_cmd="$2" sub_log="$3"
    local pub_log
    pub_log="$(mktemp)"
    ( timeout "$DURATION" stdbuf -oL -eL bash -c "$pub_cmd" < /dev/null > "$pub_log" 2>&1 & )
    sleep 1.5
    ( timeout "$DURATION" stdbuf -oL -eL bash -c "$sub_cmd" < /dev/null > "$sub_log" 2>&1 & )
    sleep "$((DURATION + 2))"
    rm -f "$pub_log"
}

run_pair() {
    local name="$1" pub_cmd="$2" sub_cmd="$3"
    local sub_log
    sub_log="$(mktemp)"
    try_once "$pub_cmd" "$sub_cmd" "$sub_log"
    if ! has_frames "$sub_log"; then
        echo "(no frames on first attempt, retrying once...)"
        try_once "$pub_cmd" "$sub_cmd" "$sub_log"
    fi
    if has_frames "$sub_log"; then
        echo "PASS: $name ($(grep -o '[0-9]* frames,' "$sub_log" | tail -1))"
    else
        echo "FAIL: $name -- no frames received (after retry)"
        echo "---- $name log ----"
        cat "$sub_log"
        echo "--------------------"
        FAIL=1
    fi
    rm -f "$sub_log"
}

echo "=== Smoke test: rlcore at $RLCORE_IP, video $VIDEO, duration ${DURATION}s each ==="

echo "--- Python ReLink (raw, 320x240) ---"
run_pair "python-relink" \
    "python3 -u '$SCRIPT_DIR/relink_camera_pub_raw.py' '$RLCORE_IP' --video '$VIDEO'" \
    "python3 -u '$SCRIPT_DIR/relink_camera_sub_raw.py' '$RLCORE_IP'"

echo "--- Python ROS2 (raw, 320x240) ---"
if command -v ros2 >/dev/null 2>&1 || python3 -c "import rclpy" 2>/dev/null; then
    run_pair "python-ros2" \
        "python3 -u '$SCRIPT_DIR/ros2_camera_pub.py' --video '$VIDEO'" \
        "python3 -u '$SCRIPT_DIR/ros2_camera_sub.py'"
else
    echo "SKIP: python-ros2 -- rclpy not available/sourced"
fi

echo "--- C++ ReLink (JPEG, 640x480) ---"
CPP_BIN_DIR="$CPP_DIR/examples/ros2_compare"
if [ -x "$CPP_BIN_DIR/relink_camera_pub_raw" ] && [ -x "$CPP_BIN_DIR/relink_camera_sub_raw" ]; then
    run_pair "cpp-relink" \
        "'$CPP_BIN_DIR/relink_camera_pub_raw' '$RLCORE_IP' --width 640 --height 480 --compressed --video '$VIDEO'" \
        "'$CPP_BIN_DIR/relink_camera_sub_raw' '$RLCORE_IP' --width 640 --height 480 --compressed"
else
    echo "SKIP: cpp-relink -- not built. Build with:"
    echo "  g++ -std=c++17 -O2 -I $CPP_DIR/relink/include -pthread $CPP_BIN_DIR/relink_camera_pub_raw.cpp -o $CPP_BIN_DIR/relink_camera_pub_raw \$(pkg-config --cflags --libs opencv4)"
    echo "  g++ -std=c++17 -O2 -I $CPP_DIR/relink/include -pthread $CPP_BIN_DIR/relink_camera_sub_raw.cpp -o $CPP_BIN_DIR/relink_camera_sub_raw \$(pkg-config --cflags --libs opencv4)"
fi

echo "--- C++ ROS2 (JPEG, 640x480) ---"
ROS2_CPP_BIN="$CPP_DIR/ros2_compare/ws/install/relink_bench_compare/lib/relink_bench_compare"
if [ -x "$ROS2_CPP_BIN/cam_pub" ] && [ -x "$ROS2_CPP_BIN/cam_sub" ]; then
    run_pair "cpp-ros2" \
        "'$ROS2_CPP_BIN/cam_pub' --width 640 --height 480 --compressed --video '$VIDEO'" \
        "'$ROS2_CPP_BIN/cam_sub' --width 640 --height 480 --compressed"
else
    echo "SKIP: cpp-ros2 -- not built. Build with:"
    echo "  cd $CPP_DIR/ros2_compare/ws && colcon build --packages-select relink_bench_compare"
fi

echo "=== Smoke test done ==="
exit "$FAIL"

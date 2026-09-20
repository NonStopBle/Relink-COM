#!/usr/bin/env bash
# Cross-language interop check for rl_topic's --ipc flag: proves a
# same-host shared-memory message published by ONE language's rl_topic
# can be received by the OTHER language's rl_topic through the same
# shm ring -- i.e. that topic_id_for()'s name hash (cpp/tools/rl_topic.cpp
# and python/relink_py/relink/cli/rl_topic.py) and shm_transport's ring
# wire format (shm_transport.hpp / shm_transport.py) actually agree,
# not just that each language's --ipc path works in isolation (which
# cpp/tests/test_rl_topic_cli.cpp and relink_py/tests/test_relink.py's
# rl_topic CLI checks already cover, standalone).
#
# Like cpp/tests/two_process_pub.cpp / two_process_sub.cpp, this is a
# manual/local check, not wired into ctest or test_relink.py: neither
# language's own test suite has (or should gain) a build/runtime
# dependency on the other language's toolchain.
#
# Usage: ./rl_topic_ipc_interop_test.sh
# Requires: cpp/build/rl_topic already built (cd cpp && cmake -B build .
# && cmake --build build --target rl_topic), and python3 able to import
# the relink package from python/relink_py (no install needed).

set -u
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CPP_RL_TOPIC="$REPO_ROOT/cpp/build/rl_topic"
PY_DIR="$REPO_ROOT/python/relink_py"
FAILURES=0

if [ ! -x "$CPP_RL_TOPIC" ]; then
    echo "FAIL: $CPP_RL_TOPIC not found -- build it first: cd cpp && cmake -B build . && cmake --build build --target rl_topic" >&2
    exit 1
fi

expected_hex() {
    # Matches both languages' echo output format (space-separated lowercase
    # hex, C++ with a trailing space, Python without -- compared as a
    # substring so that difference doesn't matter).
    python3 -c "import sys; print(' '.join(f'{b:02x}' for b in sys.argv[1].encode()))" "$1"
}

check() {
    if [ "$1" -eq 0 ]; then
        echo "ok: $2"
    else
        echo "FAIL: $2" >&2
        FAILURES=$((FAILURES + 1))
    fi
}

# --- direction 1: C++ publishes, Python echoes ---
TOPIC1="/relink/interop_test_cpp_to_py_$$_$RANDOM"
TEXT1="hello-from-cpp"
OUT1="$(mktemp)"
(cd "$PY_DIR" && timeout 10 python3 -m relink.cli.rl_topic echo "$TOPIC1" --ipc -n 1) > "$OUT1" 2>/dev/null &
PY_PID=$!
sleep 0.5
"$CPP_RL_TOPIC" pub "$TOPIC1" --ipc --text "$TEXT1" >/dev/null 2>&1
wait "$PY_PID"
grep -qF "$(expected_hex "$TEXT1")" "$OUT1"
check $? "cpp pub --ipc -> python echo --ipc received matching payload"
rm -f "$OUT1"

# --- direction 2: Python publishes, C++ echoes ---
TOPIC2="/relink/interop_test_py_to_cpp_$$_$RANDOM"
TEXT2="hello-from-python"
OUT2="$(mktemp)"
timeout 10 "$CPP_RL_TOPIC" echo "$TOPIC2" --ipc -n 1 > "$OUT2" 2>/dev/null &
CPP_PID=$!
sleep 0.5
(cd "$PY_DIR" && python3 -m relink.cli.rl_topic pub "$TOPIC2" --ipc --text "$TEXT2") >/dev/null 2>&1
wait "$CPP_PID"
grep -qF "$(expected_hex "$TEXT2")" "$OUT2"
check $? "python pub --ipc -> cpp echo --ipc received matching payload"
rm -f "$OUT2"

echo
if [ "$FAILURES" -eq 0 ]; then
    echo "ALL PASS"
    exit 0
else
    echo "$FAILURES FAILURE(S)"
    exit 1
fi

#!/usr/bin/env bash
# Example build script for ReLink's C++ core -- a thin wrapper around
# the cmake commands documented in cpp/CMakeLists.txt and the main
# README's "Building the C++ library, per OS" section. Not required
# (`cmake -B build . && cmake --build build` works on its own) -- this
# just saves typing the common variations and fails loudly with a clear
# message instead of a raw cmake error when a prerequisite is missing.
#
# Usage:
#   ./build.sh                 # native build (Linux/macOS, or Windows via MSYS2/MinGW shell)
#   ./build.sh --windows       # cross-compile for Windows from Linux/WSL with MinGW-w64
#   ./build.sh --test          # native build, then run ctest
#   ./build.sh --clean         # remove build/ and build-win/ first
#   ./build.sh --no-examples   # skip cpp/examples/*
#   ./build.sh --no-tests      # skip cpp/tests/*
#   ./build.sh --help
#
# Run from anywhere -- it cd's to this script's own directory (cpp/)
# before doing anything else, so `path/to/cpp/build.sh` works the same
# as `cd path/to/cpp && ./build.sh`.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

WINDOWS=0
RUN_TESTS=0
CLEAN=0
CMAKE_ARGS=()

for arg in "$@"; do
    case "$arg" in
        --windows)
            WINDOWS=1
            ;;
        --test|--tests)
            RUN_TESTS=1
            ;;
        --clean)
            CLEAN=1
            ;;
        --no-examples)
            CMAKE_ARGS+=("-DRELINK_BUILD_EXAMPLES=OFF")
            ;;
        --no-tests)
            CMAKE_ARGS+=("-DRELINK_BUILD_TESTS=OFF")
            ;;
        --help|-h)
            sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "build.sh: unknown argument '$arg' (see --help)" >&2
            exit 1
            ;;
    esac
done

if [[ "$WINDOWS" -eq 1 ]]; then
    BUILD_DIR="build-win"
    if ! command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1; then
        echo "build.sh: x86_64-w64-mingw32-g++ not found." >&2
        echo "  Install it with: sudo apt-get install -y g++-mingw-w64-x86-64-posix" >&2
        exit 1
    fi
    CMAKE_ARGS+=("-DCMAKE_TOOLCHAIN_FILE=$SCRIPT_DIR/cmake_example/mingw-w64-toolchain.cmake")
else
    BUILD_DIR="build"
fi

if [[ "$CLEAN" -eq 1 ]]; then
    echo "build.sh: removing build/ and build-win/"
    rm -rf build build-win
fi

if ! command -v cmake >/dev/null 2>&1; then
    echo "build.sh: cmake not found -- install it first (e.g. 'sudo apt-get install -y cmake')" >&2
    exit 1
fi

echo "build.sh: configuring into $BUILD_DIR/ (${CMAKE_ARGS[*]:-no extra options})"
cmake -B "$BUILD_DIR" "${CMAKE_ARGS[@]}" .

echo "build.sh: building"
cmake --build "$BUILD_DIR" -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

if [[ "$RUN_TESTS" -eq 1 ]]; then
    if [[ "$WINDOWS" -eq 1 ]]; then
        echo "build.sh: --test has no effect with --windows (cross-compiled binaries can't run natively here -- see the README's Wine-based verification notes instead)" >&2
    else
        echo "build.sh: running ctest"
        ctest --test-dir "$BUILD_DIR" --output-on-failure
    fi
fi

echo "build.sh: done -- binaries are under $BUILD_DIR/ (daemon under $BUILD_DIR/rlcore/, pub/sub examples under $BUILD_DIR/src/)"

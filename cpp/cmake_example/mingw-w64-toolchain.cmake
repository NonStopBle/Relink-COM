# CMake toolchain file for cross-compiling this example FROM Linux/WSL
# TO Windows with MinGW-w64 -- the CMake equivalent of running
# `x86_64-w64-mingw32-g++` directly (see the README's per-OS build
# steps). Requires: sudo apt-get install -y g++-mingw-w64-x86-64-posix
#
# Usage: cmake -B build -DCMAKE_TOOLCHAIN_FILE=mingw-w64-toolchain.cmake .

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER   x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER  x86_64-w64-mingw32-windres)

# Only look for libraries/headers/programs in the target (MinGW) sysroot,
# never fall back to the host's Linux ones.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

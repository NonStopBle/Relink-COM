# Minimal, self-contained CMake project for building against ReLink's
# header-only C++ core, on Linux, macOS, and Windows (MinGW-w64 or
# MSVC) from the same file. Copy this whole directory (CMakeLists.txt,
# main.cpp, mingw-w64-toolchain.cmake) as a starting point for your own
# project -- only RELINK_INCLUDE_DIR and the add_executable() source
# list need to change. See README.md in this directory for the full
# walkthrough, including how to build rlcore/rl_topic for it to talk to.
#
# Usage:
#   Linux / macOS:
#     cmake -B build .
#     cmake --build build
#     ./build/relink_app
#
#   Windows, native (MSYS2/MinGW-w64 shell or Visual Studio):
#     cmake -B build .
#     cmake --build build
#     build\relink_app.exe
#
#   Windows, cross-compiled from Linux/WSL with MinGW-w64:
#     cmake -B build -DCMAKE_TOOLCHAIN_FILE=mingw-w64-toolchain.cmake .
#     cmake --build build
#     wine build/relink_app.exe   # or copy to a real Windows machine

cmake_minimum_required(VERSION 3.16)
project(relink_app_example CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Points at this repo's header-only core -- in your OWN project
# (outside this repo), change this to wherever you've vendored/copied
# ReLink's cpp/include/ directory instead.
set(RELINK_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../include)

add_executable(relink_app main.cpp)
target_include_directories(relink_app PRIVATE ${RELINK_INCLUDE_DIR})

if(WIN32)
    # Winsock -- required on Windows, no equivalent needed on
    # Linux/macOS since sockets are already part of libc there.
    target_link_libraries(relink_app PRIVATE ws2_32)
    # Static-link the MinGW runtime (libgcc/libstdc++/libwinpthread) so
    # the resulting .exe runs standalone -- without this, it silently
    # depends on DLLs (libwinpthread-1.dll, etc.) that MinGW installs
    # into its own bin/ but a plain Windows machine (or a fresh Wine
    # prefix) won't have, and the .exe fails to even start.
    target_link_options(relink_app PRIVATE -static)
else()
    # pthread -- required on Linux for the UDP receive thread; macOS
    # links pthreads by default but accepting the flag there is harmless.
    find_package(Threads REQUIRED)
    target_link_libraries(relink_app PRIVATE Threads::Threads)
endif()

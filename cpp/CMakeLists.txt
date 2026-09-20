# Top-level CMake project for ReLink's C++ core -- builds the header-
# only library target, cpp/rlcore/'s daemon + relay, cpp/tools/
# (rl_topic, relink_example, relink_benchmark), and every example/test
# under cpp/examples/ and cpp/tests/, all from one `cmake -B build .`
# in this directory.
#
# Usage:
#   cmake -B build .
#   cmake --build build
#   ctest --test-dir build          # runs the unit tests
#
# Options:
#   -DRELINK_BUILD_EXAMPLES=OFF     # skip cpp/examples/*
#   -DRELINK_BUILD_TESTS=OFF        # skip cpp/tests/*
#   -DRELINK_ENABLE_XDP=ON          # relink-relay's AF_XDP fast path (see rlcore/CMakeLists.txt)
#
# See cpp/cmake_example/ for a minimal standalone template if you're
# consuming ReLink from your own separate CMake project instead of
# building inside this repo.

cmake_minimum_required(VERSION 3.16)
project(relink_cpp CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2")

option(RELINK_BUILD_EXAMPLES "Build cpp/examples/*.cpp" ON)
option(RELINK_BUILD_TESTS "Build cpp/tests/*.cpp" ON)

# Header-only core as an INTERFACE target -- every executable below
# links against `relink` to get the include path and the right
# platform libs (ws2_32 on Windows, pthread elsewhere) with no
# per-target boilerplate repeated at every add_executable() call.
add_library(relink INTERFACE)
target_include_directories(relink INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(relink INTERFACE cxx_std_17)
if(WIN32)
    target_link_libraries(relink INTERFACE ws2_32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(relink INTERFACE Threads::Threads)
    # shm_transport.hpp's shm_open()/shm_unlink() live in librt on
    # glibc < 2.34 (folded into libc itself from 2.34 on) -- link it
    # unconditionally on non-Windows rather than version-detecting, it's
    # a no-op on systems that don't need it.
    find_library(RELINK_RT_LIB rt)
    if(RELINK_RT_LIB)
        target_link_libraries(relink INTERFACE ${RELINK_RT_LIB})
    endif()
endif()

# relink-rlcore / relink-relay (own CMakeLists.txt, also usable
# standalone -- see Step 9's "Building it" in the README).
add_subdirectory(rlcore)

add_executable(rl_topic tools/rl_topic.cpp)
target_link_libraries(rl_topic PRIVATE relink)

add_executable(relink_example tools/relink_example.cpp)
target_link_libraries(relink_example PRIVATE relink)

add_executable(relink_benchmark tools/relink_benchmark.cpp)
target_link_libraries(relink_benchmark PRIVATE relink)

# relink_image_benchmark needs OpenCV (same as camera_stream below) --
# only built when it's actually found, independent of
# RELINK_BUILD_EXAMPLES since this is a tool, not an example.
find_package(OpenCV QUIET)
if(OpenCV_FOUND)
    add_executable(relink_image_benchmark tools/relink_image_benchmark.cpp)
    target_link_libraries(relink_image_benchmark PRIVATE relink ${OpenCV_LIBS})
    target_include_directories(relink_image_benchmark PRIVATE ${OpenCV_INCLUDE_DIRS})
else()
    message(STATUS "relink: OpenCV not found -- skipping relink_image_benchmark tool (see README Step 12)")
endif()

# cpp/src/ -- a minimal pub/sub quickstart (see src/README.md), separate
# from cpp/examples/'s larger message-type-coverage set. The pubsub.cpp
# FILE is named the same as examples/pubsub.cpp, but the executable
# TARGET is named src_pubsub to avoid a CMake target-name collision --
# both binaries still land in the build directory under their own
# subfolder-qualified path.
add_executable(pub src/pub.cpp)
target_link_libraries(pub PRIVATE relink)

add_executable(sub src/sub.cpp)
target_link_libraries(sub PRIVATE relink)

# main.cpp is functionally identical to pubsub.cpp (conventional
# src/main.cpp entry point) -- its own target, not reusing src_pubsub's
# object files, so it can diverge later without affecting pubsub.cpp.
add_executable(src_main src/main.cpp)
target_link_libraries(src_main PRIVATE relink)
set_target_properties(src_main PROPERTIES OUTPUT_NAME main RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/src)

add_executable(src_pubsub src/pubsub.cpp)
target_link_libraries(src_pubsub PRIVATE relink)
set_target_properties(src_pubsub PROPERTIES OUTPUT_NAME pubsub RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/src)
set_target_properties(pub sub PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/src)

if(RELINK_BUILD_EXAMPLES)
    set(RELINK_EXAMPLES
        hello_relink
        pubsub
        talker
        listener
        multicast_pubsub
        largescale_topics
        std_msgs_pubsub
        geometry_msgs_pubsub
        sensor_msgs_pubsub
        nav_msgs_pubsub
        diagnostic_msgs_pubsub
        trajectory_msgs_pubsub
        actionlib_msgs_pubsub
        standard_msgs_pubsub
        builtin_types_pubsub
        custom_types_pubsub)
    foreach(ex ${RELINK_EXAMPLES})
        add_executable(${ex} examples/${ex}.cpp)
        target_link_libraries(${ex} PRIVATE relink)
    endforeach()

    # camera_stream.cpp needs OpenCV, installed separately on every OS
    # (see Step 8) -- only built when it's actually found, so the rest
    # of this project configures and builds fine without it.
    find_package(OpenCV QUIET)
    if(OpenCV_FOUND)
        add_executable(camera_stream examples/camera_stream.cpp)
        target_link_libraries(camera_stream PRIVATE relink ${OpenCV_LIBS})
        target_include_directories(camera_stream PRIVATE ${OpenCV_INCLUDE_DIRS})
    else()
        message(STATUS "relink: OpenCV not found -- skipping camera_stream example (see README Step 8)")
    endif()
endif()

if(RELINK_BUILD_TESTS)
    enable_testing()

    # Self-contained unit tests -- safe to run unattended via ctest.
    set(RELINK_UNIT_TESTS
        test_wire
        test_frame
        test_ring_buffer
        test_topic_hash
        test_beacon
        test_register
        test_multicast_discovery
        test_udp_transport
        test_image
        test_compressed_image
        test_adaptive_bitrate
        test_relink_node
        test_shm_transport)
    foreach(t ${RELINK_UNIT_TESTS})
        add_executable(${t} tests/${t}.cpp)
        target_link_libraries(${t} PRIVATE relink)
        add_test(NAME ${t} COMMAND ${t})
    endforeach()

    # Multi-process/interactive CLIs (need to be run in pairs, or with
    # manual arguments/another process on the other end) -- built for
    # convenience, but deliberately not registered as ctest cases.
    set(RELINK_MULTIPROCESS_TESTS
        two_process_pub
        two_process_sub
        nat_punch_test
        register_client_cli)
    foreach(t ${RELINK_MULTIPROCESS_TESTS})
        add_executable(${t} tests/${t}.cpp)
        target_link_libraries(${t} PRIVATE relink)
    endforeach()
endif()

# Top-level CMake project for ReLink's C++ core -- builds the header-
# only library target, cpp/rlcore/'s daemon + relay, cpp/rl_topic.cpp,
# and every example/test under cpp/examples/ and cpp/tests/, all from
# one `cmake -B build .` in this directory.
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
target_include_directories(relink INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/relink/include)
target_compile_features(relink INTERFACE cxx_std_17)
if(WIN32)
    target_link_libraries(relink INTERFACE ws2_32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(relink INTERFACE Threads::Threads)
endif()

# relink-rlcore / relink-relay (own CMakeLists.txt, also usable
# standalone -- see Step 9's "Building it" in the README).
add_subdirectory(rlcore)

add_executable(rl_topic rl_topic.cpp)
target_link_libraries(rl_topic PRIVATE relink)

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
        test_relink_node)
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

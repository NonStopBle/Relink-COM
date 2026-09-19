cmake_minimum_required(VERSION 3.16)
project(relink_rlcore CXX)

# ---------------------------------------------------------------------------
# Build:
#   cmake -B build .                     # plain sockets only
#   cmake -B build -DRELINK_ENABLE_XDP=ON .   # + AF_XDP fast path for relink-relay
#   cmake --build build
#
# RELINK_ENABLE_XDP requires libbpf (dev headers) and clang (to compile the
# eBPF kernel object). When ON but a dependency is missing, configure fails
# with a message naming exactly what to install, rather than silently
# building without it.
# ---------------------------------------------------------------------------

option(RELINK_ENABLE_XDP "Build relink-relay with the AF_XDP fast path (requires libbpf-dev + clang)" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2")

set(RELINK_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../include)

add_executable(relink-rlcore relink_rlcore.cpp)
target_include_directories(relink-rlcore PRIVATE ${RELINK_INCLUDE_DIR})

add_executable(relink-relay relink_relay.cpp)
target_include_directories(relink-relay PRIVATE ${RELINK_INCLUDE_DIR})

if(WIN32)
    target_link_libraries(relink-rlcore PRIVATE ws2_32)
    target_link_libraries(relink-relay PRIVATE ws2_32)
endif()

if(RELINK_ENABLE_XDP)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBBPF libbpf)
    if(NOT LIBBPF_FOUND)
        message(FATAL_ERROR
            "RELINK_ENABLE_XDP=ON but libbpf was not found via pkg-config.\n"
            "Install it, e.g.:\n"
            "  sudo apt-get install -y libbpf-dev libelf-dev\n"
            "then re-run cmake.")
    endif()

    find_program(CLANG_EXECUTABLE clang)
    if(NOT CLANG_EXECUTABLE)
        message(FATAL_ERROR
            "RELINK_ENABLE_XDP=ON but clang was not found (needed to compile "
            "the eBPF kernel program to bytecode).\n"
            "Install it, e.g.:\n"
            "  sudo apt-get install -y clang llvm\n"
            "then re-run cmake.")
    endif()

    # Detect libbpf's XDP-attach API generation: libbpf >= 1.0 renamed
    # bpf_set_link_xdp_fd() to bpf_xdp_attach()/bpf_xdp_detach(). Both this
    # dev machine (libbpf 1.3.0) and the deploy target (a VPS still on
    # libbpf 0.5.0) need to build from the same source, so probe for the
    # symbol instead of hardcoding a version cutoff.
    include(CheckSymbolExists)
    set(CMAKE_REQUIRED_INCLUDES ${LIBBPF_INCLUDE_DIRS})
    set(CMAKE_REQUIRED_LIBRARIES ${LIBBPF_LIBRARIES})
    set(CMAKE_REQUIRED_LINK_DIRECTORIES ${LIBBPF_LIBRARY_DIRS})
    check_symbol_exists(bpf_xdp_attach "bpf/libbpf.h" RELINK_LIBBPF_HAS_XDP_ATTACH)
    unset(CMAKE_REQUIRED_INCLUDES)
    unset(CMAKE_REQUIRED_LIBRARIES)
    unset(CMAKE_REQUIRED_LINK_DIRECTORIES)

    # Older system kernel headers (e.g. the VPS's 5.4) predate `enum
    # bpf_stats_type`, which libbpf's userspace bpf.h forward-declares --
    # a hard C++ error without a full definition (see relay_xdp.hpp).
    # Newer kernel headers (this dev machine's) already define it, where
    # supplying it ourselves too would be a duplicate-definition error
    # instead -- so probe for which case we're in.
    include(CheckCXXSourceCompiles)
    check_cxx_source_compiles("
        #include <linux/bpf.h>
        int main() { return BPF_STATS_RUN_TIME; }
    " RELINK_KERNEL_HEADERS_HAVE_BPF_STATS_TYPE)
    if(NOT RELINK_KERNEL_HEADERS_HAVE_BPF_STATS_TYPE)
        target_compile_definitions(relink-relay PRIVATE RELINK_NEED_BPF_STATS_TYPE_SHIM)
    endif()

    # Kernel-side eBPF object: compiled once at build time with clang
    # -target bpf, installed next to the relink-relay binary since
    # RelayXdp::load_and_attach_prog() loads "relay_xdp_kern.o" from the
    # current directory at runtime.
    set(XDP_KERN_SRC ${CMAKE_CURRENT_SOURCE_DIR}/xdp/relay_xdp_kern.c)
    set(XDP_KERN_OBJ ${CMAKE_CURRENT_BINARY_DIR}/relay_xdp_kern.o)
    add_custom_command(
        OUTPUT ${XDP_KERN_OBJ}
        COMMAND ${CLANG_EXECUTABLE} -O2 -g -target bpf -D__TARGET_ARCH_x86
                -I/usr/include -I/usr/include/${CMAKE_LIBRARY_ARCHITECTURE}
                -c ${XDP_KERN_SRC} -o ${XDP_KERN_OBJ}
        DEPENDS ${XDP_KERN_SRC}
        COMMENT "Compiling relay_xdp_kern.c to eBPF bytecode"
        VERBATIM)
    add_custom_target(relay_xdp_kern ALL DEPENDS ${XDP_KERN_OBJ})
    add_dependencies(relink-relay relay_xdp_kern)

    target_compile_definitions(relink-relay PRIVATE RELINK_ENABLE_XDP)
    if(RELINK_LIBBPF_HAS_XDP_ATTACH)
        target_compile_definitions(relink-relay PRIVATE RELINK_LIBBPF_HAS_XDP_ATTACH)
    endif()
    target_include_directories(relink-relay PRIVATE ${LIBBPF_INCLUDE_DIRS})
    target_link_directories(relink-relay PRIVATE ${LIBBPF_LIBRARY_DIRS})
    target_link_libraries(relink-relay PRIVATE ${LIBBPF_LIBRARIES})

    # xsk.h (AF_XDP's user-space ring API) moved between libbpf
    # generations: older libbpf (the VPS's 0.5.0) bundles it at
    # bpf/xsk.h; newer distros split it into a separate libxdp package,
    # same API, headers at xdp/xsk.h. Detect whichever this system has.
    find_path(RELINK_BPF_XSK_H_DIR bpf/xsk.h HINTS ${LIBBPF_INCLUDE_DIRS})
    if(RELINK_BPF_XSK_H_DIR)
        message(STATUS "AF_XDP: using bpf/xsk.h from libbpf")
    else()
        pkg_check_modules(LIBXDP libxdp)
        find_path(RELINK_XDP_XSK_H_DIR xdp/xsk.h HINTS ${LIBXDP_INCLUDE_DIRS})
        if(NOT LIBXDP_FOUND OR NOT RELINK_XDP_XSK_H_DIR)
            message(FATAL_ERROR
                "RELINK_ENABLE_XDP=ON but xsk.h (AF_XDP's socket API) was "
                "not found -- neither bpf/xsk.h (older libbpf) nor "
                "xdp/xsk.h (newer libxdp). Install it, e.g.:\n"
                "  sudo apt-get install -y libxdp-dev\n"
                "then re-run cmake.")
        endif()
        message(STATUS "AF_XDP: using xdp/xsk.h from libxdp")
        target_compile_definitions(relink-relay PRIVATE RELINK_XSK_H_IS_XDP_XSK)
        target_include_directories(relink-relay PRIVATE ${LIBXDP_INCLUDE_DIRS})
        target_link_directories(relink-relay PRIVATE ${LIBXDP_LIBRARY_DIRS})
        target_link_libraries(relink-relay PRIVATE ${LIBXDP_LIBRARIES})
    endif()

    install(FILES ${XDP_KERN_OBJ} DESTINATION bin)
endif()

install(TARGETS relink-rlcore relink-relay RUNTIME DESTINATION bin)

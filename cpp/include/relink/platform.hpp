// Thin cross-platform shim over BSD sockets so the rest of the codebase
// can stay written against one API (POSIX-shaped) on both Linux and
// Windows. Only what ReLink actually uses is covered here -- this is
// not a general-purpose sockets abstraction.
//
// Windows lacks: unistd.h's close() (use closesocket()), SIGPIPE-style
// fd-based sockets (Windows' SOCKET is not an int and errors report via
// WSAGetLastError(), not errno), sendmsg()/struct iovec/struct msghdr
// (emulated below by copying into one buffer and calling sendto() --
// not zero-copy on Windows, unlike the POSIX path), and
// pthread_setaffinity_np()/SCHED_FIFO (mapped to the nearest Windows
// equivalents, best-effort, same as the POSIX side already treats a
// failed real-time request as non-fatal).
//
// macOS/BSD share the POSIX branch below (real sockets, real sendmsg)
// but also lack pthread_setaffinity_np()/cpu_set_t (a glibc-only
// extension) -- pin_thread_to_core() is a no-op there, same
// non-fatal-fallback policy as everywhere else in this file.

#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// NOGDI: windows.h's GDI header (wingdi.h) declares global functions
// named Polygon, Rectangle, Arc, etc. that collide with ReLink's own
// geometry_msgs::Polygon and friends once `using namespace
// geometry_msgs;` is in scope -- ReLink never touches GDI, so exclude
// it entirely rather than requiring every caller to fully qualify
// relink::geometry_msgs::Polygon just to dodge a Windows header.
#ifndef NOGDI
#define NOGDI
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")

namespace relink {

using socket_t = SOCKET;
using ssize_t = long long;
inline constexpr socket_t kInvalidSocket = INVALID_SOCKET;

// One Winsock init per process. `inline` gives this a single definition
// across every translation unit that includes this header (C++17), and
// its constructor runs before any socket() call in main() -- no manual
// "call this first" step needed anywhere else in the codebase.
struct WinsockGuard {
    WinsockGuard() {
        WSADATA wsa{};
        ::WSAStartup(MAKEWORD(2, 2), &wsa);
    }
    ~WinsockGuard() { ::WSACleanup(); }
};
inline WinsockGuard g_winsock_guard{};

inline int close_socket(socket_t s) { return ::closesocket(s); }
inline int last_socket_error() { return ::WSAGetLastError(); }

// Windows' SO_RCVTIMEO takes a DWORD of milliseconds, not a
// struct timeval like POSIX -- both sides of the codebase call this
// instead of setsockopt(..., SO_RCVTIMEO, ...) directly.
inline void set_recv_timeout_ms(socket_t s, long ms) {
    DWORD timeout_ms = static_cast<DWORD>(ms);
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
}

// Windows has no SO_REUSEPORT; SO_REUSEADDR there already permits
// multiple sockets bound to the same port (unlike Linux, where the two
// options are distinct), so nothing needs to be set for the
// same-host-multiple-nodes case this is used for elsewhere.
inline void enable_port_reuse_if_available(socket_t) {}

// Minimal iovec/msghdr equivalents so udp_transport.hpp's scattered
// send can stay written the same way on both platforms.
struct iovec {
    void* iov_base;
    size_t iov_len;
};

// No true scatter-gather send on Windows without WSASendMsg (which
// needs a runtime function-pointer lookup via WSAIoctl) -- instead,
// copy every iovec segment into one stack buffer and send it with a
// single sendto(). Payloads here are always well under 1500 bytes
// (kMaxPayloadBytes + framing), so this never allocates.
inline ssize_t sendmsg_to(socket_t s, const iovec* iov, int iovcnt,
                           const sockaddr_in& dest) {
    uint8_t buf[1500];
    size_t off = 0;
    for (int i = 0; i < iovcnt; ++i) {
        if (off + iov[i].iov_len > sizeof(buf)) return -1;
        std::memcpy(buf + off, iov[i].iov_base, iov[i].iov_len);
        off += iov[i].iov_len;
    }
    int sent = ::sendto(s, reinterpret_cast<const char*>(buf), static_cast<int>(off), 0,
                         reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
    return sent;
}

// Best-effort thread pinning/priority -- mirrors the POSIX side's
// "continue at normal priority/affinity if the request fails" policy.
inline void pin_thread_to_core(int core) {
    if (core < 0) return;
    ::SetThreadAffinityMask(::GetCurrentThread(), static_cast<DWORD_PTR>(1ull << core));
}
inline void set_thread_realtime(int /*priority*/) {
    // Windows has no direct SCHED_FIFO-with-priority-N equivalent
    // available without admin rights; time-critical is the closest
    // best-effort mapping.
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
}

} // namespace relink

#else // POSIX

#include <sys/socket.h>
#include <sys/uio.h>
#include <sched.h>
#include <pthread.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <cstdio>

namespace relink {

using socket_t = int;
using ::ssize_t;
inline constexpr socket_t kInvalidSocket = -1;

inline int close_socket(socket_t s) { return ::close(s); }
inline int last_socket_error() { return errno; }

inline void set_recv_timeout_ms(socket_t s, long ms) {
    struct timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

inline void enable_port_reuse_if_available(socket_t s) {
#ifdef SO_REUSEPORT
    int one = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#else
    (void)s;
#endif
}

using ::iovec;

inline ssize_t sendmsg_to(socket_t s, const iovec* iov, int iovcnt,
                           const sockaddr_in& dest) {
    struct msghdr msg{};
    msg.msg_name = const_cast<sockaddr_in*>(&dest);
    msg.msg_namelen = sizeof(dest);
    msg.msg_iov = const_cast<iovec*>(iov);
    msg.msg_iovlen = iovcnt;
    return ::sendmsg(s, &msg, 0);
}

inline void pin_thread_to_core(int core) {
    if (core < 0) return;
#ifdef __linux__
    // cpu_set_t/CPU_SET/pthread_setaffinity_np are a glibc (Linux)
    // extension -- macOS/BSD have no equivalent thread-to-core pinning
    // API at all, so this is a no-op there, same "continue unpinned"
    // policy as a failed request anywhere else.
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    ::pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
#else
    (void)core;
#endif
}
inline void set_thread_realtime(int priority) {
    struct sched_param param{};
    param.sched_priority = priority;
    int rc = ::pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
    if (rc != 0) {
        std::fprintf(stderr,
            "UdpTransport: SCHED_FIFO request failed (%s) -- continuing at normal "
            "scheduling priority. Run with CAP_SYS_NICE or as root to enable it.\n",
            std::strerror(rc));
    }
}

} // namespace relink

#endif

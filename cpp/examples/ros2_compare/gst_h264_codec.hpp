// Minimal GPU H264 encode/decode wrapper around `gst-launch-1.0`
// subprocesses, used by relink_camera_pub_raw.cpp / relink_camera_sub_raw.cpp
// under --h264. No GStreamer C API / dev headers required -- only the
// gst-launch-1.0 CLI tool and the vaapi plugin set, which ship as
// regular (non-dev) packages. This trades a little process/pipe
// overhead for zero extra build dependencies.
//
// Framing (the actual hard part of piping H264 over a byte-stream
// pipe): the encoder is run with `aud=true`, which makes vaapih264enc
// prefix every access unit (one encoded video frame, which may itself
// contain several NAL units such as SPS/PPS/slice) with an Access Unit
// Delimiter NAL (start code 00 00 00 01, NAL header byte 0x09). That
// gives a reliable, self-describing frame boundary marker inside the
// raw byte stream -- GstH264Encoder scans for it and emits exactly one
// callback per complete access unit, regardless of how the underlying
// pipe happens to chunk reads/writes.
//
// The decoder side doesn't need this trick: its output is raw BGR
// video at a fixed, known size per frame (width*height*3), so framing
// is just "read exactly that many bytes."
//
// Driver quirk discovered on this machine's Intel iHD VAAPI driver:
// vaapih264enc's `tune=low-power` property reliably breaks caps
// negotiation (surface-attribute query failure) -- omitted here. Low
// latency is instead achieved via rate-control=cqp (no encoder-side
// rate-control lookahead) and a short key-int-max.
//
// Latency cost of the AUD-boundary framing itself: since a frame is
// only known to be COMPLETE once the encoder starts writing the NEXT
// frame's AUD marker, this design adds roughly one full frame period
// of latency by construction (~33ms at 30fps) on top of the actual
// encode/decode work -- measured end-to-end at ~45-55ms vs ~8-10ms for
// JPEG at the same 1920x1080/30fps on this machine. Bytes/frame are
// far smaller than JPEG (real inter-frame prediction vs re-encoding
// each frame from scratch), so this is a real bandwidth-vs-latency
// tradeoff, not a strictly-better option -- pick JPEG when latency
// matters more than bitrate, H264 when bitrate matters more than the
// extra ~1 frame of latency.

#pragma once
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace gst_h264 {

struct ChildProcess {
    pid_t pid = -1;
    int stdin_fd = -1;   // parent writes here -> child's stdin
    int stdout_fd = -1;  // parent reads here <- child's stdout

    ChildProcess() = default;
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ChildProcess(ChildProcess&& other) noexcept { *this = std::move(other); }
    ChildProcess& operator=(ChildProcess&& other) noexcept {
        if (this != &other) {
            pid = other.pid;
            stdin_fd = other.stdin_fd;
            stdout_fd = other.stdout_fd;
            other.pid = -1;
            other.stdin_fd = -1;
            other.stdout_fd = -1;
        }
        return *this;
    }

    // Uses posix_spawn rather than fork()+exec(): by the time this
    // runs, the process already has other threads alive (OpenCV's
    // video backend starts its own internal worker threads on
    // VideoCapture::open()), and a raw fork() in a multithreaded
    // process only clones the calling thread -- any lock another
    // thread happened to hold (libc allocator, GStreamer/OpenCV
    // internals) stays locked forever in the child, which can then
    // hang before it ever reaches exec() (observed directly on this
    // codebase: the child never appeared as its target process name,
    // meaning it deadlocked between fork() and execvp()).
    // posix_spawn is specified to avoid this hazard.
    static ChildProcess spawn(const std::vector<std::string>& argv) {
        int in_pipe[2], out_pipe[2];
        if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) {
            throw std::runtime_error("gst_h264: pipe() failed");
        }

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, in_pipe[0], STDIN_FILENO);
        posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, in_pipe[0]);
        posix_spawn_file_actions_addclose(&actions, in_pipe[1]);
        posix_spawn_file_actions_addclose(&actions, out_pipe[0]);
        posix_spawn_file_actions_addclose(&actions, out_pipe[1]);

        std::vector<char*> args;
        for (auto& s : argv) args.push_back(const_cast<char*>(s.c_str()));
        args.push_back(nullptr);

        pid_t pid;
        int rc = posix_spawnp(&pid, args[0], &actions, nullptr, args.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        if (rc != 0) throw std::runtime_error("gst_h264: posix_spawnp failed: " + std::string(strerror(rc)));

        close(in_pipe[0]);
        close(out_pipe[1]);
        ChildProcess p;
        p.pid = pid;
        p.stdin_fd = in_pipe[1];
        p.stdout_fd = out_pipe[0];
        return p;
    }

    void close_stdin() {
        if (stdin_fd >= 0) { close(stdin_fd); stdin_fd = -1; }
    }

    ~ChildProcess() {
        close_stdin();
        if (stdout_fd >= 0) close(stdout_fd);
        if (pid > 0) { kill(pid, SIGTERM); waitpid(pid, nullptr, 0); }
    }
};

inline bool write_all(int fd, const uint8_t* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

inline bool read_all(int fd, uint8_t* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = read(fd, data + off, len - off);
        if (n <= 0) return false;  // EOF or error
        off += static_cast<size_t>(n);
    }
    return true;
}

// Encodes raw BGR frames to H264 via VAAPI (GPU). One push_frame() call
// per raw frame in; on_encoded_frame fires once per complete access
// unit out, asynchronously from a background thread, as soon as the
// encoder produces it (not batched).
class Encoder {
public:
    Encoder(int width, int height, double fps, int gop,
            std::function<void(const uint8_t*, size_t)> on_encoded_frame)
        : width_(width), height_(height), on_frame_(std::move(on_encoded_frame)) {
        int fps_num = static_cast<int>(fps + 0.5);
        if (fps_num < 1) fps_num = 1;
        // Note: fdsrc followed by a bare capsfilter looked like it should
        // work but reliably corrupted every frame on this machine's
        // GStreamer 1.24.2 -- videoconvert would throw "code not
        // implemented" / "invalid video buffer received" on nearly
        // every buffer, because a plain capsfilter after fdsrc doesn't
        // give the raw byte stream proper per-frame buffer metadata.
        // `rawvideoparse` is the element actually meant for "chop this
        // raw byte stream into correctly-tagged video frames" and fixes
        // it completely (verified: exact frame count in, exact AUD
        // count out, byte-identical decode).
        proc_ = ChildProcess::spawn({
            "gst-launch-1.0", "-q",
            "fdsrc", "fd=0",
            "!", "rawvideoparse", "format=bgr",
                 "width=" + std::to_string(width),
                 "height=" + std::to_string(height),
                 "framerate=" + std::to_string(fps_num) + "/1",
            "!", "videoconvert",
            "!", "vaapipostproc",
            "!", "vaapih264enc", "aud=true", "rate-control=cqp",
                 "keyframe-period=" + std::to_string(gop),
            "!", "h264parse", "config-interval=-1",
            "!", "video/x-h264,stream-format=byte-stream,alignment=au",
            "!", "fdsink", "fd=1", "sync=false",
        });
        reader_ = std::thread([this] { read_loop(); });
    }

    ~Encoder() {
        proc_.close_stdin();
        if (reader_.joinable()) reader_.join();
    }

    // Raw frame must be exactly width*height*3 bytes (BGR, no padding).
    bool push_frame(const uint8_t* bgr, size_t len) {
        return write_all(proc_.stdin_fd, bgr, len);
    }

private:
    void read_loop() {
        static const uint8_t AUD[5] = {0, 0, 0, 1, 0x09};
        std::vector<uint8_t> buf;
        size_t frame_start = std::string::npos;
        uint8_t chunk[1 << 16];
        while (true) {
            ssize_t n = read(proc_.stdout_fd, chunk, sizeof(chunk));
            if (n <= 0) break;
            buf.insert(buf.end(), chunk, chunk + n);

            while (true) {
                size_t search_from = (frame_start == std::string::npos) ? 0 : frame_start + 1;
                if (search_from + sizeof(AUD) > buf.size()) break;
                auto it = std::search(buf.begin() + search_from, buf.end(),
                                       AUD, AUD + sizeof(AUD));
                if (it == buf.end()) break;
                size_t pos = static_cast<size_t>(it - buf.begin());
                if (frame_start != std::string::npos) {
                    on_frame_(buf.data() + frame_start, pos - frame_start);
                }
                frame_start = pos;
            }
            if (frame_start != std::string::npos && frame_start > 0) {
                buf.erase(buf.begin(), buf.begin() + frame_start);
                frame_start = 0;
            }
        }
        // Flush whatever's left as the final frame.
        if (frame_start != std::string::npos && frame_start < buf.size()) {
            on_frame_(buf.data() + frame_start, buf.size() - frame_start);
        }
    }

    int width_, height_;
    std::function<void(const uint8_t*, size_t)> on_frame_;
    ChildProcess proc_;
    std::thread reader_;
};

// Decodes H264 access units back to raw BGR frames via VAAPI (GPU).
// Feed complete access units (as produced by Encoder, or received over
// the network) to push_nal(); on_decoded_frame fires once per decoded
// frame, asynchronously, from a background thread.
class Decoder {
public:
    Decoder(int width, int height, std::function<void(const uint8_t*, size_t)> on_decoded_frame)
        : width_(width), height_(height), frame_size_(static_cast<size_t>(width) * height * 3),
          on_frame_(std::move(on_decoded_frame)) {
        proc_ = ChildProcess::spawn({
            "gst-launch-1.0", "-q",
            "fdsrc", "fd=0",
            "!", "h264parse",
            "!", "vaapih264dec",
            "!", "videoconvert",
            "!", "video/x-raw,format=BGR",
            "!", "fdsink", "fd=1", "sync=false",
        });
        reader_ = std::thread([this] { read_loop(); });
    }

    ~Decoder() {
        proc_.close_stdin();
        if (reader_.joinable()) reader_.join();
    }

    bool push_nal(const uint8_t* data, size_t len) {
        return write_all(proc_.stdin_fd, data, len);
    }

private:
    void read_loop() {
        std::vector<uint8_t> frame(frame_size_);
        while (read_all(proc_.stdout_fd, frame.data(), frame_size_)) {
            on_frame_(frame.data(), frame_size_);
        }
    }

    int width_, height_;
    size_t frame_size_;
    std::function<void(const uint8_t*, size_t)> on_frame_;
    ChildProcess proc_;
    std::thread reader_;
};

}  // namespace gst_h264

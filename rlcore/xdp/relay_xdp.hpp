// Userspace AF_XDP fast path for relink-relay.
//
// Only compiled in when RELINK_ENABLE_XDP is defined (opt-in build
// flag -- see README/CMake). Loads relay_xdp_kern.o onto a NIC, opens
// an AF_XDP socket bound to one RX queue, and drives its UMEM/fill/
// completion/RX/TX rings directly per docs.kernel.org/networking/
// af_xdp.html -- packets matching the relay port never enter the
// kernel UDP stack at all; this replaces recvfrom/sendto on the hot
// path, not relink-relay's control-packet (REGISTER) handling, which
// stays on a normal fallback socket since it's not latency-sensitive.
//
// Falls back to returning false from RelayXdp::init() if the kernel/
// driver/toolchain don't support it, so the caller can drop back to
// plain sockets rather than crash.
#pragma once

// This VPS's kernel headers (5.4) predate `enum bpf_stats_type`, which
// libbpf's bpf.h references in a forward declaration -- fine for C's
// looser opaque-enum rule, a hard error in C++. Supply it ourselves
// (matches upstream's only-ever value) before libbpf's headers use it.
#ifndef __cplusplus
#else
enum bpf_stats_type { BPF_STATS_RUN_TIME = 0 };
#endif
#include <bpf/libbpf.h>
#include <bpf/xsk.h>
#include <bpf/bpf.h>
#include <linux/if_link.h> // XDP_FLAGS_DRV_MODE / XDP_FLAGS_SKB_MODE (libbpf 0.5's bpf_set_link_xdp_fd API)
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <net/if.h>
#include <poll.h>
#include <unistd.h>
#include <sys/mman.h>

namespace relink {

class RelayXdp {
public:
    static constexpr uint32_t kNumFrames = 4096;
    static constexpr uint32_t kFrameSize = XSK_UMEM__DEFAULT_FRAME_SIZE; // 4096
    static constexpr uint32_t kRingSize = 2048;

    // ifname: the NIC to attach to (e.g. "eth0"); relay_port: the UDP
    // port relink-relay listens on, written into relay_port_map so
    // the kernel program filters for it without recompiling.
    bool init(const char* ifname, uint16_t relay_port) {
        ifindex_ = if_nametoindex(ifname);
        if (ifindex_ == 0) {
            std::perror("if_nametoindex");
            return false;
        }

        // xsk_umem__create requires page-aligned memory (it's shared
        // with the kernel via the ring descriptors) -- plain `new`
        // heap allocation isn't guaranteed aligned and fails with
        // EINVAL, so mmap it directly instead.
        void* area = ::mmap(nullptr, kNumFrames * kFrameSize, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (area == MAP_FAILED) {
            std::perror("mmap umem");
            return false;
        }
        umem_area_ = static_cast<uint8_t*>(area);

        struct xsk_umem_config umem_cfg = {};
        umem_cfg.fill_size = kNumFrames; // must hold every frame we seed below
        umem_cfg.comp_size = kRingSize;
        umem_cfg.frame_size = kFrameSize;
        umem_cfg.frame_headroom = XSK_UMEM__DEFAULT_FRAME_HEADROOM;
        int ret = xsk_umem__create(&umem_, umem_area_, kNumFrames * kFrameSize,
                                    &fill_ring_, &comp_ring_, &umem_cfg);
        if (ret) {
            std::fprintf(stderr, "xsk_umem__create failed: %s\n", std::strerror(-ret));
            return false;
        }

        // Seed the fill ring with every frame up front -- the kernel
        // uses these to land incoming packet data; we hand frames back
        // to it as soon as we're done reading each one (see rx()).
        uint32_t idx = 0;
        ret = xsk_ring_prod__reserve(&fill_ring_, kNumFrames, &idx);
        if (ret != static_cast<int>(kNumFrames)) {
            std::fprintf(stderr, "fill ring reserve failed\n");
            return false;
        }
        for (uint32_t i = 0; i < kNumFrames; ++i) {
            *xsk_ring_prod__fill_addr(&fill_ring_, idx + i) = i * kFrameSize;
        }
        xsk_ring_prod__submit(&fill_ring_, kNumFrames);

        struct xsk_socket_config sock_cfg = {};
        sock_cfg.rx_size = kRingSize;
        sock_cfg.tx_size = kRingSize;
        sock_cfg.libbpf_flags = 0;
        sock_cfg.xdp_flags = 0; // let libbpf pick native, falls back to SKB mode automatically
        sock_cfg.bind_flags = XDP_USE_NEED_WAKEUP;
        ret = xsk_socket__create(&xsk_, ifname, /*queue_id=*/0, umem_,
                                  &rx_ring_, &tx_ring_, &sock_cfg);
        if (ret) {
            std::fprintf(stderr, "xsk_socket__create failed: %s\n", std::strerror(-ret));
            return false;
        }

        if (!load_and_attach_prog(ifname, relay_port)) {
            return false;
        }

        xsk_fd_ = xsk_socket__fd(xsk_);
        return true;
    }

    // Blocks (via poll()) until at least one frame is available, then
    // hands back a pointer directly into UMEM -- no copy -- via `out`/
    // `out_len`. Caller must call release_rx() with the same idx when
    // done so the frame returns to the fill ring for reuse.
    bool poll_rx(int timeout_ms) {
        struct pollfd pfd = {xsk_fd_, POLLIN, 0};
        return ::poll(&pfd, 1, timeout_ms) > 0;
    }

    uint32_t rx_batch(uint32_t max, uint32_t* out_idx_rx) {
        return xsk_ring_cons__peek(&rx_ring_, max, out_idx_rx);
    }

    void rx_frame(uint32_t idx_rx, uint8_t** data, uint32_t* len) {
        const struct xdp_desc* desc = xsk_ring_cons__rx_desc(&rx_ring_, idx_rx);
        *data = static_cast<uint8_t*>(xsk_umem__get_data(umem_area_, desc->addr));
        *len = desc->len;
        last_rx_addr_ = desc->addr;
    }

    void release_rx(uint32_t nb) {
        xsk_ring_cons__release(&rx_ring_, nb);
    }

    // Re-fills one frame address back into the fill ring so the NIC
    // can reuse that UMEM slot for a future incoming packet.
    void refill(uint64_t addr) {
        uint32_t idx;
        if (xsk_ring_prod__reserve(&fill_ring_, 1, &idx) == 1) {
            *xsk_ring_prod__fill_addr(&fill_ring_, idx) = addr;
            xsk_ring_prod__submit(&fill_ring_, 1);
        }
    }

    // Detaches the XDP program from the NIC. MUST be called before the
    // process exits (normal return or a signal) -- an attached XDP
    // program outlives the process that loaded it and keeps
    // redirecting matching traffic into a socket that no longer
    // exists, silently blackholing that port until someone notices
    // and runs `ip link set dev <if> xdp off` by hand. detach() is
    // idempotent so a signal handler can call it unconditionally.
    void detach() {
        if (attached_ && ifindex_) {
            bpf_set_link_xdp_fd(ifindex_, -1, XDP_FLAGS_DRV_MODE);
            bpf_set_link_xdp_fd(ifindex_, -1, XDP_FLAGS_SKB_MODE);
            attached_ = false;
        }
    }

    ~RelayXdp() {
        detach();
        if (xsk_) xsk_socket__delete(xsk_);
        if (umem_) xsk_umem__delete(umem_);
        if (umem_area_) ::munmap(umem_area_, kNumFrames * kFrameSize);
        if (bpf_obj_) bpf_object__close(bpf_obj_);
    }

private:
    bool load_and_attach_prog(const char* ifname, uint16_t relay_port) {
        struct bpf_object* obj = bpf_object__open_file("relay_xdp_kern.o", nullptr);
        if (libbpf_get_error(obj)) {
            std::fprintf(stderr, "bpf_object__open_file failed (need relay_xdp_kern.o next to the binary)\n");
            return false;
        }
        if (bpf_object__load(obj)) {
            std::fprintf(stderr, "bpf_object__load failed\n");
            return false;
        }
        bpf_obj_ = obj;

        struct bpf_program* prog = bpf_object__find_program_by_name(obj, "relay_xdp_prog");
        if (!prog) { std::fprintf(stderr, "prog not found\n"); return false; }
        int prog_fd = bpf_program__fd(prog);

        int port_map_fd = bpf_object__find_map_fd_by_name(obj, "relay_port_map");
        int xsks_map_fd = bpf_object__find_map_fd_by_name(obj, "xsks_map");
        if (port_map_fd < 0 || xsks_map_fd < 0) {
            std::fprintf(stderr, "maps not found\n");
            return false;
        }
        uint32_t key = 0;
        uint16_t port_val = relay_port;
        bpf_map_update_elem(port_map_fd, &key, &port_val, BPF_ANY);

        int queue_id = 0;
        int xsk_fd = xsk_socket__fd(xsk_);
        bpf_map_update_elem(xsks_map_fd, &queue_id, &xsk_fd, BPF_ANY);

        // Try native driver mode first (virtio_net supports it); fall
        // back to generic/SKB mode so this still works on NICs that
        // don't -- slower, but correct. (libbpf 0.5's API here is
        // bpf_set_link_xdp_fd, predating the bpf_xdp_attach rename.)
        int err = bpf_set_link_xdp_fd(ifindex_, prog_fd, XDP_FLAGS_DRV_MODE);
        if (err) {
            std::fprintf(stderr, "native XDP attach failed (%s), falling back to generic mode\n",
                         std::strerror(-err));
            err = bpf_set_link_xdp_fd(ifindex_, prog_fd, XDP_FLAGS_SKB_MODE);
            if (err) {
                std::fprintf(stderr, "generic XDP attach also failed: %s\n", std::strerror(-err));
                return false;
            }
        }
        attached_ = true;
        return true;
    }

    unsigned int ifindex_ = 0;
    bool attached_ = false;
    struct xsk_umem* umem_ = nullptr;
    struct xsk_socket* xsk_ = nullptr;
    struct xsk_ring_prod fill_ring_ = {};
    struct xsk_ring_cons comp_ring_ = {};
    struct xsk_ring_cons rx_ring_ = {};
    struct xsk_ring_prod tx_ring_ = {};
    uint8_t* umem_area_ = nullptr;
    struct bpf_object* bpf_obj_ = nullptr;
    int xsk_fd_ = -1;
    uint64_t last_rx_addr_ = 0;

public:
    unsigned int ifindex() const { return ifindex_; }
    bool is_attached() const { return attached_; }
};

} // namespace relink

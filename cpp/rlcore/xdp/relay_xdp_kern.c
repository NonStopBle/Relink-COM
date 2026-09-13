// XDP program for relink-relay's AF_XDP fast path.
//
// Runs in the kernel driver's RX hook (virtio_net supports native XDP,
// verified on the target VPS), before the normal socket stack even
// allocates an skb. All it does is: parse just far enough to see the
// UDP destination port, and if it matches the relay's port, redirect
// the raw packet into an AF_XDP socket via XSKMAP -- skipping the
// kernel UDP/IP receive path entirely for relay traffic. Everything
// else (SSH, other services) falls through untouched.
//
// Fan-out to N subscribers still happens in userspace (relay_xdp.hpp)
// -- native XDP has no multi-destination redirect primitive, so this
// program's only job is fast, cheap ingress filtering.
#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

struct {
    __uint(type, BPF_MAP_TYPE_XSKMAP);
    __uint(max_entries, 64);
    __uint(key_size, sizeof(int));
    __uint(value_size, sizeof(int));
} xsks_map SEC(".maps");

// Set from userspace before attach (see relay_xdp.hpp) so the program
// doesn't need to be recompiled to change the relay's port.
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u16);
} relay_port_map SEC(".maps");

SEC("xdp")
int relay_xdp_prog(struct xdp_md* ctx) {
    void* data = (void*)(long)ctx->data;
    void* data_end = (void*)(long)ctx->data_end;

    struct ethhdr* eth = data;
    if ((void*)(eth + 1) > data_end) return XDP_PASS;
    if (eth->h_proto != bpf_htons(ETH_P_IP)) return XDP_PASS;

    struct iphdr* ip = (void*)(eth + 1);
    if ((void*)(ip + 1) > data_end) return XDP_PASS;
    if (ip->protocol != IPPROTO_UDP) return XDP_PASS;

    // ip->ihl is in 32-bit words; options are rare for UDP but handle
    // them correctly rather than assuming a fixed 20-byte header.
    struct udphdr* udp = (void*)ip + (ip->ihl * 4);
    if ((void*)(udp + 1) > data_end) return XDP_PASS;

    __u32 key = 0;
    __u16* relay_port = bpf_map_lookup_elem(&relay_port_map, &key);
    if (!relay_port) return XDP_PASS;

    if (udp->dest != bpf_htons(*relay_port)) return XDP_PASS;

    // Redirect into whichever AF_XDP socket userspace bound to this
    // queue index -- bpf_redirect_map looks it up by ctx->rx_queue_index.
    return bpf_redirect_map(&xsks_map, ctx->rx_queue_index, XDP_PASS);
}

char _license[] SEC("license") = "GPL";

#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>


//Use HASH_MAP_TYPE later on for the maps
SEC("xdp") 
int ebpf_event_tx(struct xdp_md *ctx)
{
    return XDP_PASS;
}

char _license[] SEC("license") = "GPL";
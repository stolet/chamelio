#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

#include "cham_fast.h"
#include "udp_hdr.h"
#include "ip_hdr.h"
#include "eth_hdr.h"
#include "rpc.h"
#include "rpc_fast.h"
#include "rpc_queue_types.h"
#include "utils.h"

/* Add these functions as helpers */
static void * (*queue_tail)(struct equeue *q) = (void *) 1001;
static int (*queue_enqueue)(struct equeue *q, __u8 type) = (void *) 1002;

static void * (*bpf_memcpy)(void *dst, void *src, size_t len) = (void *) 1003;
static void (*bpf_print)(int a) = (void *) 1004;

static __u16 (*ipv4_checksum)(void *ip_hdr) = (void *) 1005;
static __u16 (*ipv4_udptcp_cksum)(void *ip_hdr, void *udp_hdr) = (void *) 1006;

static struct cham_sched_entry * (*sched_head)(struct cham_scheduler *sched) = (void *) 1007;
static int (*sched_pop)(struct cham_scheduler *sched) = (void *) 1008;
static int (*sched_add)(struct cham_scheduler *sched, __u32 id, __u32 priority) = (void *) 1009;


SEC("chamelio/event_rx")
int event_rx(struct cham_ebpf_ctx *ctx)
{
  return 0;
}

SEC("chamelio/event_tx")
int event_tx(struct cham_ebpf_ctx *ctx)
{
  return -1;
}

SEC("chamelio/event_deq")
int event_deq(struct cham_ebpf_ctx *ctx)
{
  return 0;
}
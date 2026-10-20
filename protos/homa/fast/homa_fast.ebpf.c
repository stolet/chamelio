#include <linux/types.h>
#include <bpf/bpf_helpers.h>

#include "cham_fast.h"

SEC("chamelio/event_rx")
int event_rx(struct cham_ebpf_ctx *ctx)
{
  return -1;
}

SEC("chamelio/event_sched")
int event_sched(struct cham_ebpf_ctx *ctx)
{
  return 0;
}

SEC("chamelio/event_deq")
int event_deq(struct cham_ebpf_ctx *ctx)
{
  return 0;
}

char LICENSE[] SEC("license") = "GPL";

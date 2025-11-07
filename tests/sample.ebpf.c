#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>

struct cham_ebpf_ctx {};

static void (*ebpf_print)(void) = (void *) 2;

SEC("chamelio/event_rx")
int event_rx(struct cham_ebpf_ctx *ctx)
{
  ebpf_print();
  return 0;
}

SEC("chamelio/event_tx")
int event_tx(struct cham_ebpf_ctx *ctx)
{
  return 0;
}

SEC("chamelio/event_deq")
int event_deq(struct cham_ebpf_ctx *ctx)
{
  return 0;
}
#ifndef CHAM_PROTOS_H_
#define CHAM_PROTOS_H_

#include "cham_fast.h"
#include "queue_types.h"
#include "log.h"

int tcp_event_rx(struct cham_ebpf_ctx *ctx);
int tcp_event_sched(struct cham_ebpf_ctx *ctx);
int tcp_event_deq(struct cham_ebpf_ctx *ctx);

int udp_event_rx(struct cham_ebpf_ctx *ctx);
int udp_event_sched(struct cham_ebpf_ctx *ctx);
int udp_event_deq(struct cham_ebpf_ctx *ctx);

static inline int proto_hand_event_rx(__u8 proto_type,
    struct cham_ebpf_ctx *ctx)
{
  switch (proto_type)
  {
    case CHAM_PROTO_TCP:
      return tcp_event_rx(ctx);
    case CHAM_PROTO_UDP:
      return udp_event_rx(ctx);
    default:
      return -1;
  }
}

static inline int proto_hand_event_sched(__u8 proto_type,
    struct cham_ebpf_ctx *ctx)
{
  switch (proto_type)
  {
    case CHAM_PROTO_TCP:
      return tcp_event_sched(ctx);
    case CHAM_PROTO_UDP:
      return udp_event_sched(ctx);
    default:
      return -1;
  }
}

static inline int proto_hand_event_deq(__u8 proto_type,
    struct cham_ebpf_ctx *ctx)
{
  switch (proto_type)
  {
    case CHAM_PROTO_TCP:
      return tcp_event_deq(ctx);
    case CHAM_PROTO_UDP:
      return udp_event_deq(ctx);
    default:
      return -1;
  }
}

#endif

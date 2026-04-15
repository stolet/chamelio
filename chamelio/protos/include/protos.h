#ifndef CHAM_PROTOS_H_
#define CHAM_PROTOS_H_

#include "cham_fast.h"

int tcp_event_rx(struct cham_ebpf_ctx *ctx);
int tcp_event_tx(struct cham_ebpf_ctx *ctx);
int tcp_event_deq(struct cham_ebpf_ctx *ctx);

int udp_event_rx(struct cham_ebpf_ctx *ctx);
int udp_event_tx(struct cham_ebpf_ctx *ctx);
int udp_event_deq(struct cham_ebpf_ctx *ctx);

#endif

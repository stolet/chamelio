#ifndef INFRA_H_
#define INFRA_H_

#include <stdlib.h>
#include <linux/types.h>

#include "fast.h"

#define INFRA_RET_OK 0
#define INFRA_RET_ERR -1
#define INFRA_RET_MBUF 1

struct guest_fast * infra_rx(struct fast_context *ctx,
    struct rte_mbuf *mb, __u64 *pkt_off);
int infra_tx(struct fast_context *ctx, 
    struct guest_fast *g, struct rte_mbuf *mb, size_t pkt_len);

#endif
#ifndef FAST_EBPF_H_
#define FAST_EBPF_H_

#include <linux/types.h>

#include <rte_mbuf.h>

#include "cham_fast.h"
#include "eth_hdr.h"
#include "ip_hdr.h"
#include "gre_hdr.h"
#include "udp.h"

static inline void fast_ebpf_ctx_set_pkt(struct cham_ebpf_ctx *ctx,
    struct rte_mbuf *mb, __u64 pkt_off)
{
  __u8 *pkt = rte_pktmbuf_mtod(mb, __u8 *) + pkt_off;

  ctx->pkt = pkt;
  ctx->pkt_end = pkt + UDP_MSS;
}

static inline void fast_ebpf_ctx_set_pkt_l2(struct cham_ebpf_ctx *ctx,
    struct rte_mbuf *mb, int virt_gre)
{
  __u8 *pkt = rte_pktmbuf_mtod(mb, __u8 *) + sizeof(struct eth_hdr);

  if (virt_gre)
    pkt += sizeof(struct ip_hdr) + sizeof(struct gre_hdr);

  ctx->pkt = pkt;
  ctx->pkt_end = pkt + UDP_MSS;
}

#endif

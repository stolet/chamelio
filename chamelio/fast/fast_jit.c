#include <stddef.h>

#include <rte_mbuf.h>

#include "fast.h"
#include "infra.h"
#include "fast_jit.h"
#include "eth_hdr.h"
#include "ip_hdr.h"
#include "gre_hdr.h"
#include "udp.h"

/* Weak stub to satisfy the host link; overridden by eBPF entry when linked. */
__attribute__((weak))
uint64_t __chamelio_ebpf(void *mem, size_t mem_len)
{
  return 0;
}

uint64_t fast_jit_rx(void *mem, size_t mem_len)
{
  struct fast_jit_rx_ctx *ctx = (struct fast_jit_rx_ctx *) mem;
  struct guest_fast *g = ctx->g;

  g->proto.ebpf_ctx.pkt = rte_pktmbuf_mtod(ctx->mb, __u8 *) + ctx->pkt_off;
  g->proto.ebpf_ctx.pkt_end = (void *) (g->proto.ebpf_ctx.pkt + UDP_MSS);
  (void) __chamelio_ebpf(&g->proto.ebpf_ctx, sizeof(struct cham_ebpf_ctx));

  return 1;
}

uint64_t fast_jit_deq(void *mem, size_t mem_len)
{
  int deq_ret;
  int ret;
  struct fast_jit_deq_ctx *ctx = (struct fast_jit_deq_ctx *) mem;
  struct fast_context *f_ctx = ctx->f_ctx;
  struct guest_fast *g = ctx->g;
  struct rte_mbuf *mb = ctx->mb;

  mb->data_off = 0;
  g->proto.ebpf_ctx.pkt = rte_pktmbuf_mtod(mb, __u8 *) +
      sizeof(struct eth_hdr);
  if (f_ctx->config->virt_gre)
  {
    g->proto.ebpf_ctx.pkt = g->proto.ebpf_ctx.pkt +
      sizeof(struct ip_hdr) + sizeof(struct gre_hdr);
  }
  g->proto.ebpf_ctx.pkt_end = (void *) ((__u64) rte_pktmbuf_mtod(mb, __u8 *) +
      UDP_MSS);

  g->proto.ebpf_ctx.qe = ctx->qe;
  g->proto.ebpf_ctx.qid = ctx->qid;

  deq_ret = (int) __chamelio_ebpf(&g->proto.ebpf_ctx,
      sizeof(struct cham_ebpf_ctx));

  if (deq_ret > 0)
  {
    ret = infra_tx(f_ctx, g, mb, deq_ret);
    if (ret == 0)
    {
      f_ctx->tx_mbs[f_ctx->tx_n] = mb;
      f_ctx->tx_n++;
      return 1;
    }
  }

  return 0;
}

uint64_t fast_jit_tx(void *mem, size_t mem_len)
{
  int tx_ret;
  int ret;
  struct fast_jit_tx_ctx *ctx = (struct fast_jit_tx_ctx *) mem;
  struct fast_context *f_ctx = ctx->f_ctx;
  struct guest_fast *g = ctx->g;
  struct rte_mbuf *mb = ctx->mb;

  mb->data_off = 0;
  g->proto.ebpf_ctx.pkt = rte_pktmbuf_mtod(mb, __u8 *) +
      sizeof(struct eth_hdr);
  if (f_ctx->config->virt_gre)
  {
    g->proto.ebpf_ctx.pkt = g->proto.ebpf_ctx.pkt +
      sizeof(struct ip_hdr) + sizeof(struct gre_hdr);
  }
  g->proto.ebpf_ctx.pkt_end = (void *) ((__u64) rte_pktmbuf_mtod(mb, __u8 *) +
      UDP_MSS);

  tx_ret = (int) __chamelio_ebpf(&g->proto.ebpf_ctx.pkt,
      sizeof(struct cham_ebpf_ctx));

  if (tx_ret < 0)
    return (uint64_t) -1;

  ret = infra_tx(f_ctx, g, mb, tx_ret);
  if (ret == 0)
  {
    f_ctx->tx_mbs[f_ctx->tx_n] = mb;
    f_ctx->tx_n++;
    return 1;
  }

  return 0;
}

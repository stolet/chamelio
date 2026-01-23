#include <stddef.h>

#include <rte_mbuf.h>

#include "fast.h"
#include "infra.h"
#include "fast_comb.h"
#include "fast_ebpf.h"
#include "eth_hdr.h"
#include "ip_hdr.h"
#include "gre_hdr.h"
#include "udp.h"

/* Weak stub to satisfy the host link; overridden by eBPF entry when linked. */
__attribute__((weak))
uint64_t __cham_comb(void *mem, size_t mem_len)
{
  return 0;
}

uint64_t fast_comb_rx(struct fast_comb_rx_ctx *ctx, size_t mem_len)
{
  struct guest_fast *g = ctx->g;

  fast_ebpf_ctx_set_pkt(&g->proto.ebpf_ctx, ctx->mb, ctx->pkt_off);
  (void) __cham_comb(&g->proto.ebpf_ctx, sizeof(struct cham_ebpf_ctx));

  return 1;
}

uint64_t fast_comb_deq(struct fast_comb_deq_ctx *ctx, size_t mem_len)
{
  int deq_ret;
  int ret;
  struct fast_context *f_ctx = ctx->f_ctx;
  struct guest_fast *g = ctx->g;
  struct rte_mbuf *mb = ctx->mb;

  mb->data_off = 0;
  fast_ebpf_ctx_set_pkt_l2(&g->proto.ebpf_ctx, mb, f_ctx->config->virt_gre);

  g->proto.ebpf_ctx.qe = ctx->qe;
  g->proto.ebpf_ctx.qid = ctx->qid;

  deq_ret = (int) __cham_comb(&g->proto.ebpf_ctx,
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

uint64_t fast_comb_tx(struct fast_comb_tx_ctx *ctx, size_t mem_len)
{
  int tx_ret;
  int ret;
  struct fast_context *f_ctx = ctx->f_ctx;
  struct guest_fast *g = ctx->g;
  struct rte_mbuf *mb = ctx->mb;

  mb->data_off = 0;
  fast_ebpf_ctx_set_pkt_l2(&g->proto.ebpf_ctx, mb, f_ctx->config->virt_gre);

  tx_ret = (int) __cham_comb(&g->proto.ebpf_ctx,
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

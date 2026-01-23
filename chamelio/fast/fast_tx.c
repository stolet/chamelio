#include <stdlib.h>
#include <linux/types.h>

#include <rte_mbuf.h>

#include "fast.h"
#include "fast_comb.h"
#include "ip_hdr.h"
#include "gre_hdr.h"
#include "udp.h"
#include "txcache.h"
#include "clock.h"
#include "infra.h"
#include "ebpf.h"

static inline int tx_poll_guest(struct fast_context *ctx,
    struct guest_fast *g, struct rte_mbuf *mb, int *ntx);
static inline int tx_poll_guest_comb(struct fast_context *ctx,
    struct guest_fast *g, struct rte_mbuf *mb, int *ntx);

int fast_tx_poll(struct fast_context *ctx)
{
  unsigned max;
  int i, ntx;
  struct guest_fast *g;
  struct rte_mbuf **mbs;
  __u64 tsc_start, tsc_spent;
  __u8 n_guests = ctx->n_guests;
  
  /* Return if no guests have registered */
  if (ctx->guests == NULL)
    return 0;

  max = FAST_BATCH_SIZE;
  if (TXBUF_SIZE - ctx->tx_n < max)
    max = TXBUF_SIZE - ctx->tx_n;

  /* Allocate mbufs to use for transmission from mempool cache */
  max = txcache_alloc(ctx, &mbs, max);

  g = ctx->guests;
  ntx = 0;
  for (i = 0; i < n_guests && g != NULL && ntx < max; i++)
  {
    tsc_start = clock_rdtsc();
    g = &ctx->guests[i];

    /* Continue to next guest if ebpf code hasn't been uploaded yet */
    if (g->proto.event_tx_vm == NULL)
      continue;
    
    /* Continue to next guest if out of budget */
    if (__atomic_load_n(g->budget, __ATOMIC_RELAXED) <= 0)
      continue;

    for (;ntx < max;)
    {
      if (ctx->config->fp_jit_combined)
      {
        if (tx_poll_guest_comb(ctx, g, mbs[ntx], &ntx) < 0)
          break;
        continue;
      }

      if (tx_poll_guest(ctx, g, mbs[ntx], &ntx) < 0)
        break;
    }

    /* Subtract guest's budget */
    tsc_spent = clock_rdtsc() - tsc_start;
    __atomic_fetch_sub(g->budget, tsc_spent, __ATOMIC_RELAXED);
  }

  fast_txflush(ctx);

  /* Free buffers that were not used */
  for (i = 0; i < max; i++)
    txcache_free(ctx, mbs[i]);

  return 0;
}

static inline int tx_poll_guest(struct fast_context *ctx,
    struct guest_fast *g, struct rte_mbuf *mb, int *ntx)
{
  int tx_ret;
  int ret;

  /* Prepare packet */
  mb->data_off = 0;
  g->proto.ebpf_ctx.pkt = rte_pktmbuf_mtod(mb, __u8 *) +
      sizeof(struct eth_hdr);
  if (ctx->config->virt_gre)
  {
    g->proto.ebpf_ctx.pkt = g->proto.ebpf_ctx.pkt +
      sizeof(struct ip_hdr) + sizeof(struct gre_hdr);
  }
  g->proto.ebpf_ctx.pkt_end = (void *) ((__u64) rte_pktmbuf_mtod(mb, __u8 *) +
      UDP_MSS);
  
  /* Execute custom protocol tx procedure */
  ebpf_vm_exec(g->proto.event_tx_vm, &g->proto.ebpf_ctx.pkt, 
      sizeof(struct cham_ebpf_ctx), &tx_ret);

  if (tx_ret < 0)
  {
    return -1;
  }

  /* Add destination MAC address */
  ret = infra_tx(ctx, g, mb, tx_ret);

  /* TODO: Don't drop packet if ARP lookup hasn't resolved */
  if (ret == 0)
  {
    /* Add to transmission buffer if packet processed for TX */
    ctx->tx_mbs[ctx->tx_n] = mb;
    ctx->tx_n++;
    (*ntx)++;
  }

  return 0;
}

static inline int tx_poll_guest_comb(struct fast_context *ctx,
    struct guest_fast *g, struct rte_mbuf *mb, int *ntx)
{
  int tx_ret;
  struct fast_comb_tx_ctx jit_ctx = {
    .f_ctx = ctx,
    .g = g,
    .mb = mb,
  };

  ebpf_vm_exec(g->proto.event_tx_vm, &jit_ctx, sizeof(jit_ctx), &tx_ret);

  if (tx_ret < 0)
    return -1;
  if (tx_ret > 0)
    (*ntx)++;
  return 0;
}
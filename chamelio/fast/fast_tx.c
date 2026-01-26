#include <stdlib.h>
#include <linux/types.h>

#include <rte_mbuf.h>

#include "fast.h"
#include "fast_comb.h"
#include "fast_ebpf.h"
#include "ip_hdr.h"
#include "gre_hdr.h"
#include "udp.h"
#include "txcache.h"
#include "clock.h"
#include "infra.h"
#include "ebpf.h"

static inline int tx_poll_guest(struct fast_context *ctx,
    struct guest_fast *g, struct rte_mbuf **mbs, int max, int *ntx);
static inline int tx_poll_guest_comb(struct fast_context *ctx,
    struct guest_fast *g, struct rte_mbuf **mbs, int max, int *ntx);

int fast_tx_poll(struct fast_context *ctx)
{
  int max;
  int i, ntx;
  struct guest_fast *g;
  struct rte_mbuf **mbs;
  __u8 n_guests = ctx->n_guests;
  const int use_comb = ctx->config->fp_jit_combined;
  
  /* Return if no guests have registered */
  if (ctx->guests == NULL)
    return 0;

  max = FAST_BATCH_SIZE;
  if (TXBUF_SIZE - ctx->tx_n < max)
    max = TXBUF_SIZE - ctx->tx_n;

  /* Allocate mbufs to use for transmission from mempool cache */
  max = txcache_alloc(ctx, &mbs, max);
  if (max == 0)
    return 0;

  /* Prefetch first two mbuf cachelines */
  utils_prefetch0(rte_pktmbuf_mtod(mbs[0], __u8 *));
  utils_prefetch0(rte_pktmbuf_mtod(mbs[0] + 64, __u8 *));

  ntx = 0;
  for (i = 0; i < n_guests && ntx < max; i++)
  {
    /* Prefetch next mbuf two cachelines */
    if (ntx + 1 < max)
    {
      utils_prefetch0(rte_pktmbuf_mtod(mbs[ntx + 1], __u8 *));
      utils_prefetch0(rte_pktmbuf_mtod(mbs[ntx + 1] + 64, __u8 *));
    }

    g = &ctx->guests[i];
    if (use_comb)
      tx_poll_guest_comb(ctx, g, mbs, max, &ntx);
    else
      tx_poll_guest(ctx, g, mbs, max, &ntx);
  }

  /* Free buffers that were not used */
  fast_txflush(ctx);
  for (i = 0; i < max; i++)
    txcache_free(ctx, mbs[i]);

  return 0;
}

static inline int tx_poll_guest(struct fast_context *ctx,
    struct guest_fast *g, struct rte_mbuf **mbs, int max, int *ntx)
{
  int tx_ret;
  int ret;
  __u64 tsc_start, tsc_spent;

  /* Continue to next guest if ebpf code hasn't been uploaded yet */
  if (g->proto.event_tx_vm == NULL)
    return 0;

  /* Continue to next guest if out of budget */
  if (__atomic_load_n(g->budget, __ATOMIC_RELAXED) <= 0)
    return 0;

  tsc_start = clock_rdtsc();
  tx_ret = 0;
  while (*ntx < max)
  {
    struct rte_mbuf *mb = mbs[*ntx];

    /* Prepare packet */
    mb->data_off = 0;
    fast_ebpf_ctx_set_pkt_l2(&g->proto.ebpf_ctx, mb, ctx->config->virt_gre);
  
    /* Execute custom protocol tx procedure */
    ebpf_vm_exec(g->proto.event_tx_vm, &g->proto.ebpf_ctx,
        sizeof(struct cham_ebpf_ctx), &tx_ret);

    if (tx_ret < 0)
      break;

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
  }

  tsc_spent = clock_rdtsc() - tsc_start;
  __atomic_fetch_sub(g->budget, tsc_spent, __ATOMIC_RELAXED);

  if (tx_ret < 0)
    return -1;

  return 0;
}

static inline int tx_poll_guest_comb(struct fast_context *ctx,
    struct guest_fast *g, struct rte_mbuf **mbs, int max, int *ntx)
{
  int tx_ret;
  struct fast_comb_tx_ctx jit_ctx = {
    .f_ctx = ctx,
    .g = g,
    .mbs = mbs,
    .max = max,
    .ntx = ntx,
  };

  /* Continue to next guest if ebpf code hasn't been uploaded yet */
  if (g->proto.event_tx_vm == NULL)
    return 0;

  /* Continue to next guest if out of budget */
  if (__atomic_load_n(g->budget, __ATOMIC_RELAXED) <= 0)
    return 0;

  ebpf_vm_exec(g->proto.event_tx_vm, &jit_ctx, sizeof(jit_ctx), &tx_ret);

  if (tx_ret < 0)
    return -1;
  return 0;
}

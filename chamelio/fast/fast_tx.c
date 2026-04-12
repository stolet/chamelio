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
#include "log.h"
#include "scheduler_fns.h"

static inline int tx_poll_guest(struct fast_context *ctx,
    struct guest_fast *g, struct rte_mbuf **mbs, int max, int *ntx,
    int charge_budget);
static inline int tx_poll_guest_comb(struct fast_context *ctx,
    struct guest_fast *g, struct rte_mbuf **mbs, int max, int *ntx,
    int charge_budget);

int fast_tx_poll(struct fast_context *ctx)
{
  int max, ret;
  int i, ntx, has_tx_work;
  struct guest_fast *g;
  struct rte_mbuf **mbs;
  __u8 n_guests = ctx->n_guests;
  const int use_comb = ctx->fp_jit_combined;
  const int charge_budget = ctx->perf_iso;
  
  /* Return if no guests have registered */
  if (n_guests == 0)
    return 0;

  /* Skip tx path if no guest has both a tx ebpf vm and pending scheduler work. */
  has_tx_work = 0;
  for (i = 0; i < n_guests; i++)
  {
    g = &ctx->guests[i];
    if (g->proto.event_tx_vm == NULL)
      continue;
      
    if (sched_head(&g->proto.ebpf_ctx.sched) == NULL)
      continue;
    
    if (charge_budget && __atomic_load_n(g->budget, __ATOMIC_RELAXED) <= 0)
      continue;
      
    has_tx_work = 1;
    break;
  }

  if (!has_tx_work)
    return 0;

  max = FAST_TX_BATCH_SIZE;
  if (TXBUF_SIZE - ctx->tx_n < max)
    max = TXBUF_SIZE - ctx->tx_n;

  /* Allocate mbufs to use for transmission from mempool cache */
  max = txcache_alloc(ctx, &mbs, max);
  if (max == 0)
    return 0;

  /* Prefetch first two mbuf cachelines */
  rte_prefetch0(rte_pktmbuf_mtod(mbs[0], __u8 *));
  rte_prefetch0(rte_pktmbuf_mtod(mbs[0], __u8 *) + 64);

  ntx = 0;
  for (i = 0; i < n_guests && ntx < max; i++)
  {
    /* Prefetch next mbuf two cachelines */
    if (ntx + 1 < max)
    {
      rte_prefetch0(rte_pktmbuf_mtod(mbs[ntx + 1], __u8 *));
      rte_prefetch0(rte_pktmbuf_mtod(mbs[ntx + 1], __u8 *) + 64);
    }

    g = &ctx->guests[i];
    if (use_comb)
      ret = tx_poll_guest_comb(ctx, g, mbs, max, &ntx, charge_budget);
    else
      ret = tx_poll_guest(ctx, g, mbs, max, &ntx, charge_budget);

    (void) ret;
  }

  /* Flush TX and roll back unused mbufs in the cache */
  fast_txflush(ctx);
  txcache_unalloc(ctx, max - ntx);

  return ntx;
}

static inline int tx_poll_guest(struct fast_context *ctx,
    struct guest_fast *g, struct rte_mbuf **mbs, int max, int *ntx,
    int charge_budget)
{
  int tx_ret;
  int ret;
  __u64 tsc_start = 0, tsc_spent;

  /* Continue to next guest if ebpf code hasn't been uploaded yet */
  if (g->proto.event_tx_vm == NULL)
    return 0;

  /* Continue to next guest if there is no pending scheduler work */
  if (sched_head(&g->proto.ebpf_ctx.sched) == NULL)
    return 0;

  /* Continue to next guest if out of budget */
  if (charge_budget && __atomic_load_n(g->budget, __ATOMIC_RELAXED) <= 0)
    return 0;

  if (charge_budget)
    tsc_start = clock_rdtsc();
  tx_ret = 0;
  while (*ntx < max)
  {
    if (sched_head(&g->proto.ebpf_ctx.sched) == NULL)
      break;

    struct rte_mbuf *mb = mbs[*ntx];

    /* Prepare packet */
    mb->data_off = 0;
    fast_ebpf_ctx_set_pkt_l2(&g->proto.ebpf_ctx, mb, ctx->virt_gre);
  
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

  if (charge_budget)
  {
    tsc_spent = clock_rdtsc() - tsc_start;
    __atomic_fetch_sub(g->budget, tsc_spent, __ATOMIC_RELAXED);
  }

  if (tx_ret < 0)
    return -1;

  return 0;
}

static inline int tx_poll_guest_comb(struct fast_context *ctx,
    struct guest_fast *g, struct rte_mbuf **mbs, int max, int *ntx,
    int charge_budget)
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

  /* Continue to next guest if there is no pending scheduler work */
  if (sched_head(&g->proto.ebpf_ctx.sched) == NULL)
    return 0;

  /* Continue to next guest if out of budget */
  if (charge_budget && __atomic_load_n(g->budget, __ATOMIC_RELAXED) <= 0)
    return 0;

  ebpf_vm_exec(g->proto.event_tx_vm, &jit_ctx, sizeof(jit_ctx), &tx_ret);

  if (tx_ret < 0)
    return -1;
  return 0;
}

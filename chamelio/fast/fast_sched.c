#include <stdlib.h>
#include <linux/types.h>

#include <rte_mbuf.h>

#include "fast.h"
#include "fast_ebpf.h"
#include "ip_hdr.h"
#include "gre_hdr.h"
#include "udp.h"
#include "txcache.h"
#include "clock.h"
#include "infra.h"
#include "ebpf.h"
#include "scheduler_fns.h"
#include "protos.h"

static inline int sched_poll_guest(struct fast_context *ctx,
    struct guest_fast *g, struct rte_mbuf **mbs, int max, int *ntx,
    int charge_budget);

int fast_sched_poll(struct fast_context *ctx)
{
  int max, ret;
  int i, gid, ntx, last_sched_guest;
  struct guest_fast *g;
  struct rte_mbuf **mbs;
  __u8 n_guests = ctx->n_guests;
  __u8 start_guest = ctx->next_sched_guest;
  const int charge_budget = ctx->perf_iso;
  
  /* Return if no guests have registered */
  if (n_guests == 0)
    return 0;

  if (start_guest >= n_guests)
    start_guest = 0;

  max = FAST_SCHED_BATCH_SIZE;
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
  last_sched_guest = -1;
  for (i = 0; i < n_guests && ntx < max; i++)
  {
    /* Prefetch next mbuf two cachelines */
    if (ntx + 1 < max)
    {
      rte_prefetch0(rte_pktmbuf_mtod(mbs[ntx + 1], __u8 *));
      rte_prefetch0(rte_pktmbuf_mtod(mbs[ntx + 1], __u8 *) + 64);
    }

    gid = (start_guest + i) % n_guests;
    g = &ctx->guests[gid];
    ret = sched_poll_guest(ctx, g, mbs, max, &ntx, charge_budget);
    if (ret > 0)
      last_sched_guest = gid;
  }

  if (last_sched_guest >= 0)
    ctx->next_sched_guest = (last_sched_guest + 1) % n_guests;

  /* Flush TX and roll back unused mbufs in the cache */
  fast_txflush(ctx);
  txcache_unalloc(ctx, max - ntx);

  return ntx;
}

static inline int sched_poll_guest(struct fast_context *ctx,
    struct guest_fast *g, struct rte_mbuf **mbs,
    int max, int *ntx, int charge_budget)
{
  int exec_ret;
  int ntx_start;
  int sched_ret;
  int ret;
  int did_work;
  __u64 tsc_start = 0, tsc_spent;

  switch (ctx->fp_proto_mode)
  {
    case FP_PROTO_EBPF:
      /* Continue to next guest if ebpf code hasn't been uploaded yet */
      if (g->proto.event_sched_vm == NULL)
        return 0;
      break;
    case FP_PROTO_HAND:
      if (g->proto.proto_type == CHAM_PROTO_INVALID)
        return 0;
      break;
    default:
      return 0;
  }

  /* Continue to next guest if out of budget */
  if (charge_budget && __atomic_load_n(g->budget, __ATOMIC_RELAXED) <= 0)
    return 0;

  if (charge_budget)
    tsc_start = clock_rdtsc();
  
  ntx_start = *ntx;
  sched_ret = 0;
  did_work = 0;
  while (*ntx < max)
  {
    if (sched_head(&g->proto.ebpf_ctx.sched) == NULL)
      break;

    struct rte_mbuf *mb = mbs[*ntx];

    /* Prepare packet */
    mb->data_off = 0;
    fast_ebpf_ctx_set_pkt_l2(&g->proto.ebpf_ctx, mb, ctx->virt_gre);
  
    /* Execute custom protocol scheduler procedure */
    switch (ctx->fp_proto_mode)
    {
      case FP_PROTO_EBPF:
        exec_ret = ebpf_vm_exec(g->proto.event_sched_vm, &g->proto.ebpf_ctx,
            sizeof(struct cham_ebpf_ctx), &sched_ret);
        break;
      case FP_PROTO_HAND:
        sched_ret = proto_hand_event_sched(g->proto.proto_type,
            &g->proto.ebpf_ctx);
        exec_ret = 0;
        break;
      default:
        sched_ret = -1;
        exec_ret = -1;
        break;
    }

    if (sched_ret <= 0 || exec_ret < 0)
      break;

    /* Add destination MAC address */
    ret = infra_tx(ctx, g, mb, sched_ret);
    if (ret == INFRA_RET_OK)
    {
      ctx->tx_mbs[ctx->tx_n] = mb;
      ctx->tx_n++;
      (*ntx)++;
      did_work = 1;
    }
    else if (ret == INFRA_RET_MBUF)
    {
      (*ntx)++;
      did_work = 1;
    }
  }

  if (charge_budget && *ntx > ntx_start)
  {
    tsc_spent = clock_rdtsc() - tsc_start;
    fast_budg_charge(ctx, g, tsc_spent);
  }

  if (sched_ret < 0)
    return -1;

  return did_work;
}

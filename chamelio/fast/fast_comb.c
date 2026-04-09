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
#include "clock.h"
#include "queue_fns.h"
#include "scheduler_fns.h"

static inline void dqueue_rotate_head(struct guest_fast *g,
    struct cham_dqueue *qcur);

/* Weak stub to satisfy the host link; overridden by eBPF entry when linked. */
__attribute__((weak))
uint64_t __cham_comb(void *mem, size_t mem_len)
{
  return 0;
}

uint64_t fast_comb_rx(struct fast_comb_rx_ctx *ctx, size_t mem_len)
{
  struct guest_fast *g = ctx->g;

  fast_ebpf_ctx_set_pkt(&g->proto.ebpf_ctx, ctx->mb, ctx->pkt_off,
      ctx->virt_gre);
  (void) __cham_comb(&g->proto.ebpf_ctx, sizeof(struct cham_ebpf_ctx));

  return 1;
}

uint64_t fast_comb_deq(struct fast_comb_deq_ctx *ctx, size_t mem_len)
{
  int deq_ret;
  int ret;
  int j;
  struct fast_context *f_ctx = ctx->f_ctx;
  struct guest_fast *g = ctx->g;
  struct rte_mbuf *mb;
  struct cham_dqueue *qcur;
  struct queue_entry *qe;
  __u64 tsc_start = 0, tsc_spent;
  const int charge_budget = f_ctx->perf_iso;

  /* Continue if this guest is out of budget */
  if (charge_budget && __atomic_load_n(g->budget, __ATOMIC_RELAXED) <= 0)
    return 0;

  if (charge_budget)
    tsc_start = clock_rdtsc();
  for (j = 0; j < g->proto.ndqueues && *ctx->ndeq < ctx->max; j++)
  {
    qcur = &g->proto.dqueues[g->proto.dqueues_head];
    while (*ctx->ndeq < ctx->max)
    {
      qe = queue_head(&qcur->dq);

      /* Stop draining this queue once it is empty */
      if (qe == NULL)
        break;

      mb = ctx->mbs[*ctx->ntx];
      mb->data_off = 0;
      fast_ebpf_ctx_set_pkt_l2(&g->proto.ebpf_ctx, mb, f_ctx->virt_gre);

      g->proto.ebpf_ctx.qe = qe;
      g->proto.ebpf_ctx.qid = qcur->id;

      deq_ret = (int) __cham_comb(&g->proto.ebpf_ctx,
          sizeof(struct cham_ebpf_ctx));
      (*ctx->ndeq)++;

      if (deq_ret < 0)
        return (uint64_t) -1;

      if (deq_ret > 0)
      {
        ret = infra_tx(f_ctx, g, mb, deq_ret);
        if (ret == 0)
        {
          f_ctx->tx_mbs[f_ctx->tx_n] = mb;
          f_ctx->tx_n++;
          (*ctx->ntx)++;
        }
      }

      ret = queue_dequeue(&qcur->dq);
      if (ret != 0)
        return (uint64_t) -1;
    }

    dqueue_rotate_head(g, qcur);
  }

  /* Subtract from guest's budget */
  if (charge_budget)
  {
    tsc_spent = clock_rdtsc() - tsc_start;
    __atomic_fetch_sub(g->budget, tsc_spent, __ATOMIC_RELAXED);
  }

  return 0;
}

uint64_t fast_comb_tx(struct fast_comb_tx_ctx *ctx, size_t mem_len)
{
  int tx_ret;
  int ret;
  int max;
  int ntx;
  struct fast_context *f_ctx = ctx->f_ctx;
  struct guest_fast *g = ctx->g;
  struct rte_mbuf *mb;
  struct rte_mbuf **mbs;
  __u64 tsc_start = 0, tsc_spent;
  const int charge_budget = f_ctx->perf_iso;

  if (g->proto.event_tx_vm == NULL)
    return 0;

  if (sched_head(&g->proto.ebpf_ctx.sched) == NULL)
    return 0;

  if (charge_budget && __atomic_load_n(g->budget, __ATOMIC_RELAXED) <= 0)
    return 0;

  mbs = ctx->mbs;
  max = ctx->max;
  ntx = *ctx->ntx;

  if (charge_budget)
    tsc_start = clock_rdtsc();
  tx_ret = 0;
  while (ntx < max)
  {
    if (sched_head(&g->proto.ebpf_ctx.sched) == NULL)
      break;

    mb = mbs[ntx];
    mb->data_off = 0;
    fast_ebpf_ctx_set_pkt_l2(&g->proto.ebpf_ctx, mb, f_ctx->virt_gre);

    tx_ret = (int) __cham_comb(&g->proto.ebpf_ctx,
        sizeof(struct cham_ebpf_ctx));

    if (tx_ret < 0)
      break;

    ret = infra_tx(f_ctx, g, mb, tx_ret);
    if (ret == 0)
    {
      f_ctx->tx_mbs[f_ctx->tx_n] = mb;
      f_ctx->tx_n++;
      ntx++;
    }
  }

  if (charge_budget)
  {
    tsc_spent = clock_rdtsc() - tsc_start;
    __atomic_fetch_sub(g->budget, tsc_spent, __ATOMIC_RELAXED);
  }
  *ctx->ntx = ntx;

  if (tx_ret < 0)
    return (uint64_t) -1;

  return 0;
}

static inline void dqueue_rotate_head(struct guest_fast *g,
    struct cham_dqueue *qcur)
{
  g->proto.dqueues[g->proto.dqueues_tail].next = qcur->id;
  g->proto.dqueues_tail = qcur->id;

  /* When there's only one queue, keep head pointing to it (circular list).
     Otherwise, advance head to the next queue. */
  if (g->proto.ndqueues == 1)
    g->proto.dqueues_head = qcur->id;
  else
    g->proto.dqueues_head = qcur->next;

  qcur->next = PROTOQ_ID_INVALID;
}

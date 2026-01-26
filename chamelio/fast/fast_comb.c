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

  fast_ebpf_ctx_set_pkt(&g->proto.ebpf_ctx, ctx->mb, ctx->pkt_off);
  (void) __cham_comb(&g->proto.ebpf_ctx, sizeof(struct cham_ebpf_ctx));

  return 1;
}

uint64_t fast_comb_deq(struct fast_comb_deq_ctx *ctx, size_t mem_len)
{
  int deq_ret;
  int ret;
  int j;
  int last_queue_empty;
  struct fast_context *f_ctx = ctx->f_ctx;
  struct guest_fast *g = ctx->g;
  struct rte_mbuf *mb;
  struct cham_dqueue *qcur;
  struct queue_entry *qe;
  __u64 tsc_start, tsc_spent;

  /* Continue if this guest is out of budget */
  if (__atomic_load_n(g->budget, __ATOMIC_RELAXED) <= 0)
    return 0;

  tsc_start = clock_rdtsc();
  last_queue_empty = 1;
  for (j = 0; j < g->proto.ndqueues; j++)
  {
    qcur = &g->proto.dqueues[g->proto.dqueues_head];
    qe = queue_head(&qcur->dq);

    /* If there are no messages in queue, continue to next */
    if (qe == NULL)
    {
      dqueue_rotate_head(g, qcur);
      last_queue_empty = 1;
      continue;
    }

    last_queue_empty = 0;
    mb = ctx->mbs[*ctx->ntx];
    mb->data_off = 0;
    fast_ebpf_ctx_set_pkt_l2(&g->proto.ebpf_ctx, mb, f_ctx->config->virt_gre);

    g->proto.ebpf_ctx.qe = qe;
    g->proto.ebpf_ctx.qid = qcur->id;

    deq_ret = (int) __cham_comb(&g->proto.ebpf_ctx,
        sizeof(struct cham_ebpf_ctx));
    (*ctx->ndeq)++;

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

  /* Rotate head if last polled queue didn't increment head */
  if (!last_queue_empty)
  {
    qcur = &g->proto.dqueues[g->proto.dqueues_head];
    dqueue_rotate_head(g, qcur);
  }

  /* Subtract from guest's budget */
  tsc_spent = clock_rdtsc() - tsc_start;
  __atomic_fetch_sub(g->budget, tsc_spent, __ATOMIC_RELAXED);

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
  __u64 tsc_start, tsc_spent;

  if (g->proto.event_tx_vm == NULL)
    return 0;

  if (__atomic_load_n(g->budget, __ATOMIC_RELAXED) <= 0)
    return 0;

  mbs = ctx->mbs;
  max = ctx->max;
  ntx = *ctx->ntx;

  tsc_start = clock_rdtsc();
  tx_ret = 0;
  while (ntx < max)
  {
    mb = mbs[ntx];
    mb->data_off = 0;
    fast_ebpf_ctx_set_pkt_l2(&g->proto.ebpf_ctx, mb, f_ctx->config->virt_gre);

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

  tsc_spent = clock_rdtsc() - tsc_start;
  __atomic_fetch_sub(g->budget, tsc_spent, __ATOMIC_RELAXED);
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
  g->proto.dqueues_head = qcur->next;
  qcur->next = PROTOQ_ID_INVALID;
}

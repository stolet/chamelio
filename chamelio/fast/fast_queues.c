#include <stdlib.h>
#include <linux/types.h>

#include <rte_mbuf.h>

#include "fast.h"
#include "fast_ebpf.h"
#include "ip_hdr.h"
#include "udp_hdr.h"
#include "gre_hdr.h"
#include "udp.h"
#include "txcache.h"
#include "clock.h"
#include "queue_fns.h"
#include "queue_types.h"
#include "infra.h"
#include "ebpf.h"
#include "log.h"
#include "protos.h"

static inline int queues_poll_guest(struct fast_context *ctx,
    struct guest_fast *g, struct rte_mbuf **mbs, int max,
    int *ntx, int *ndeq, int charge_budget);
static inline int queues_poll_guest_dequeue(struct fast_context *ctx,
    struct guest_fast *g, struct cham_dqueue *qcur, struct queue_entry *qe,
    struct rte_mbuf *mb, int *ntx, int *ndeq);
static inline void dqueue_rotate_head(struct guest_fast *g,
    struct cham_dqueue *qcur);

int fast_queues_poll(struct fast_context *ctx)
{
  int i, gid, max, prev_ndeq, ret, ndeq, ntx, last_queues_guest;
  struct guest_fast *g;
  struct rte_mbuf **mbs;
  __u8 start_guest = ctx->next_queues_guest;
  const int charge_budget = ctx->perf_iso;

  if (ctx->n_guests == 0)
    return 0;

  if (start_guest >= ctx->n_guests)
    start_guest = 0;

  max = FAST_DEQ_BATCH_SIZE;
  if (TXBUF_SIZE - ctx->tx_n < max)
    max = TXBUF_SIZE - ctx->tx_n;

  /* Allocate mbufs for transmission */
  max = txcache_alloc(ctx, &mbs, max);
  if (max <= 0)
    return 0;

  /* Prefetch first two mbuf cachelines */
  rte_prefetch0(rte_pktmbuf_mtod(mbs[0], __u8 *));
  rte_prefetch0(rte_pktmbuf_mtod(mbs[0], __u8 *) + 64);

  ntx = 0;
  ndeq = 0;
  last_queues_guest = -1;
  for (i = 0; i < ctx->n_guests && ndeq < max; i++)
  {
    /* Prefetch next mbuf two cachelines */
    if (ndeq + 1 < max)
    {
      rte_prefetch0(rte_pktmbuf_mtod(mbs[ndeq + 1], __u8 *));
      rte_prefetch0(rte_pktmbuf_mtod(mbs[ndeq + 1], __u8 *) + 64);
    }

    gid = (start_guest + i) % ctx->n_guests;
    g = &ctx->guests[gid];
    prev_ndeq = ndeq;
    ret = queues_poll_guest(ctx, g, mbs, max, &ntx, &ndeq, charge_budget);

    if (ret != 0)
    {
      LOG_ERROR("failed to dequeue queue for proto=%d", g->id);
      return -1;
    }

    if (ndeq > prev_ndeq)
      last_queues_guest = gid;
  }

  if (last_queues_guest >= 0)
    ctx->next_queues_guest = (last_queues_guest + 1) % ctx->n_guests;
  
  fast_txflush(ctx);
  txcache_unalloc(ctx, max - ntx);

  return ndeq;
}

static inline int queues_poll_guest(struct fast_context *ctx,
    struct guest_fast *g, struct rte_mbuf **mbs, int max,
    int *ntx, int *ndeq, int charge_budget)
{
  int j;
  int ndeq_start;
  int ret;
  struct cham_dqueue *qcur;
  struct queue_entry *qe;
  __u64 tsc_start = 0, tsc_spent;

  /* Continue if there are no activated queues for this protocol */
  if (g->proto.dqueues_head == PROTOQ_ID_INVALID)
    return 0;
    
  /* Continue if this guest is out of budget */
  if (charge_budget && __atomic_load_n(g->budget, __ATOMIC_RELAXED) <= 0)
    return 0;

  if (charge_budget)
    tsc_start = clock_rdtsc();
  ndeq_start = *ndeq;
    
  for (j = 0; j < g->proto.ndqueues && *ndeq < max; j++)
  {
    qcur = &g->proto.dqueues[g->proto.dqueues_head];
    
    /* Drain this queue before going to the next */
    while (*ndeq < max)
    {
      qe = queue_head(&qcur->dq);

      /* Stop draining this queue once it is empty */
      if (qe == NULL)
        break;

      ret = queues_poll_guest_dequeue(ctx, g, qcur, qe, mbs[*ntx], ntx, ndeq);
      if (ret < 0)
        return -1;

      ret = queue_dequeue(&qcur->dq);
      if (ret < 0)
        return -1;
    }

    dqueue_rotate_head(g, qcur);
  }

  /* Subtract from guest's budget */
  if (charge_budget && *ndeq > ndeq_start)
  {
    tsc_spent = clock_rdtsc() - tsc_start;
    __atomic_fetch_sub(g->budget, tsc_spent, __ATOMIC_RELAXED);
    fast_budg_add(ctx, g->id, tsc_spent);
  }

  return 0;
}

static inline int queues_poll_guest_dequeue(struct fast_context *ctx,
    struct guest_fast *g, struct cham_dqueue *qcur, struct queue_entry *qe,
    struct rte_mbuf *mb, int *ntx, int *ndeq)
{
  int deq_ret;
  int ret, exec_ret;

  /* Prepare packet buffer for potential TX */
  mb->data_off = 0;
  fast_ebpf_ctx_set_pkt_l2(&g->proto.ebpf_ctx, mb, ctx->virt_gre);

  /* Add queue entry to eBPF context */
  g->proto.ebpf_ctx.qe = qe;
  g->proto.ebpf_ctx.qid = qcur->id;

  /* Execute custom dequeue procedure */
  switch (ctx->fp_proto_mode)
  {
    case FP_PROTO_EBPF:
      exec_ret = ebpf_vm_exec(g->proto.event_deq_vm, &g->proto.ebpf_ctx,
          sizeof(struct cham_ebpf_ctx), &deq_ret);
      break;
    case FP_PROTO_HAND:
      deq_ret = proto_hand_event_deq(g->proto.proto_type, &g->proto.ebpf_ctx);
      exec_ret = 0;
      break;
    default:
      deq_ret = -1;
      exec_ret = -1;
      break;
  }
  (*ndeq)++;

  if (deq_ret < 0 || exec_ret < 0)
    return -1;

  /* Add to transmission buffer if packet processed for TX */
  if (deq_ret > 0)
  {
    /* Add destination MAC address and run infra processing */
    ret = infra_tx(ctx, g, mb, deq_ret);

    /* Add to TX buffer if infra protos were successful */
    if (ret == 0)
    {
      ctx->tx_mbs[ctx->tx_n] = mb;
      ctx->tx_n++;
      (*ntx)++;
    }
  }

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

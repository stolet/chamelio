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
#include "queue_fns.h"
#include "queue_types.h"
#include "infra.h"
#include "ebpf.h"

static inline int queues_poll_guest(struct fast_context *ctx,
    struct guest_fast *g, struct rte_mbuf **mbs, int max,
    int *ntx, int *ndeq);
static inline int queues_poll_guest_comb(struct fast_context *ctx,
    struct guest_fast *g, struct rte_mbuf **mbs, int max,
    int *ntx, int *ndeq);
static inline void queues_poll_guest_dequeue(struct fast_context *ctx,
    struct guest_fast *g, struct cham_dqueue *qcur, struct queue_entry *qe,
    struct rte_mbuf *mb, int *ntx, int *ndeq);
static inline void dqueue_rotate_head(struct guest_fast *g,
    struct cham_dqueue *qcur);

int fast_queues_poll(struct fast_context *ctx)
{
  int i, max, ret, ndeq, ntx;
  struct guest_fast *g;
  struct rte_mbuf **mbs;
  const int use_comb = ctx->fp_jit_combined;

  max = FAST_BATCH_SIZE;
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
  for (i = 0; i < ctx->n_guests && ndeq < max; i++)
  {
    /* Prefetch next mbuf two cachelines */
    if (ndeq + 1 < max)
    {
      rte_prefetch0(rte_pktmbuf_mtod(mbs[ndeq + 1], __u8 *));
      rte_prefetch0(rte_pktmbuf_mtod(mbs[ndeq + 1], __u8 *) + 64);
    }

    g = &ctx->guests[i];
    if (use_comb)
      ret = queues_poll_guest_comb(ctx, g, mbs, max, &ntx, &ndeq);
    else
      ret = queues_poll_guest(ctx, g, mbs, max, &ntx, &ndeq);

    if (ret != 0)
    {
      LOG_ERROR("failed to dequeue queue for proto=%d", g->id);
      return -1;
    }
  }

  fast_txflush(ctx);
  txcache_unalloc(ctx, max - ntx);

  return 0;
}

static inline int queues_poll_guest(struct fast_context *ctx,
    struct guest_fast *g, struct rte_mbuf **mbs, int max,
    int *ntx, int *ndeq)
{
  int j;
  int ret;
  int last_queue_empty;
  struct cham_dqueue *qcur;
  struct queue_entry *qe;
  __u64 tsc_start, tsc_spent;

  /* Continue if there are no activated queues for this protocol */
  if (g->proto.dqueues_head == PROTOQ_ID_INVALID)
    return 0;

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
    queues_poll_guest_dequeue(ctx, g, qcur, qe, mbs[*ntx], ntx, ndeq);

    ret = queue_dequeue(&qcur->dq);
    if (ret != 0)
      return -1;
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

static inline void queues_poll_guest_dequeue(struct fast_context *ctx,
    struct guest_fast *g, struct cham_dqueue *qcur, struct queue_entry *qe,
    struct rte_mbuf *mb, int *ntx, int *ndeq)
{
  int deq_ret;
  int ret;

  /* Prepare packet buffer for potential TX */
  mb->data_off = 0;
  fast_ebpf_ctx_set_pkt_l2(&g->proto.ebpf_ctx, mb, ctx->virt_gre);

  /* Add queue entry to eBPF context */
  g->proto.ebpf_ctx.qe = qe;
  g->proto.ebpf_ctx.qid = qcur->id;

  /* Execute custom dequeue procedure */
  ebpf_vm_exec(g->proto.event_deq_vm, &g->proto.ebpf_ctx,
      sizeof(struct cham_ebpf_ctx), &deq_ret);
  (*ndeq)++;

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
}

static inline int queues_poll_guest_comb(struct fast_context *ctx,
    struct guest_fast *g, struct rte_mbuf **mbs, int max,
    int *ntx, int *ndeq)
{
  int deq_ret;
  struct fast_comb_deq_ctx jit_ctx = {
    .f_ctx = ctx,
    .g = g,
    .mbs = mbs,
    .max = max,
    .ntx = ntx,
    .ndeq = ndeq,
  };

  /* Continue if there are no activated queues for this protocol */
  if (g->proto.dqueues_head == PROTOQ_ID_INVALID)
    return 0;

  ebpf_vm_exec(g->proto.event_deq_vm, &jit_ctx, sizeof(jit_ctx), &deq_ret);

  if (deq_ret < 0)
    return -1;

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

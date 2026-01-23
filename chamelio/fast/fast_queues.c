#include <stdlib.h>
#include <linux/types.h>

#include <rte_mbuf.h>

#include "fast.h"
#include "fast_jit.h"
#include "ip_hdr.h"
#include "gre_hdr.h"
#include "udp.h"
#include "txcache.h"
#include "clock.h"
#include "queue_fns.h"
#include "queue_types.h"
#include "infra.h"
#include "ebpf.h"

static inline void queues_poll_guest(struct fast_context *ctx,
    struct guest_fast *g, struct cham_dqueue *qcur, struct queue_entry *qe,
    struct rte_mbuf *mb, int *ntx, int *ndeq);
static inline void queues_poll_guest_comb(struct fast_context *ctx,
    struct guest_fast *g, struct cham_dqueue *qcur, struct queue_entry *qe,
    struct rte_mbuf *mb, int *ntx, int *ndeq);

int fast_queues_poll(struct fast_context *ctx)
{
  int i, j, max, ret, ndeq, ntx;
  __u8 qcur_empty;
  struct guest_fast *g;
  struct cham_dqueue *qcur;
  struct queue_entry *qe;
  struct rte_mbuf **mbs;
  __u64 tsc_start, tsc_spent;

  max = FAST_BATCH_SIZE;
  if (TXBUF_SIZE - ctx->tx_n < max)
    max = TXBUF_SIZE - ctx->tx_n;

  /* Allocate mbufs for transmission */
  max = txcache_alloc(ctx, &mbs, max);
  if (max <= 0)
    return 0;

  ntx = 0;
  ndeq = 0;
  qcur_empty = 0;
  for (i = 0; i < ctx->n_guests && ndeq < max; i++)
  {
    tsc_start = clock_rdtsc();
    g = &ctx->guests[i];

    /* Continue if there are no activated queues for this protocol */
    if (g->proto.dqueues_head == PROTOQ_ID_INVALID)
      continue;

    /* Continue if this guest is out of budget */
    if (__atomic_load_n(g->budget, __ATOMIC_RELAXED) <= 0)
      continue;

    for (j = 0; j < g->proto.ndqueues; j++)
    {
      qcur_empty = 0;
      qcur = &g->proto.dqueues[g->proto.dqueues_head];
      qe = queue_head(&qcur->dq);

      /* If there are no messages in queue, continue to next */
      if (qe == NULL)
      {
        g->proto.dqueues[g->proto.dqueues_tail].next = qcur->id;
        g->proto.dqueues_tail = qcur->id;
        g->proto.dqueues_head = qcur->next;
        qcur->next = PROTOQ_ID_INVALID;
        qcur_empty = 1;
        continue;
      }

      if (ctx->config->fp_jit_combined)
        queues_poll_guest_comb(ctx, g, qcur, qe, mbs[ntx], &ntx, &ndeq);
      else
        queues_poll_guest(ctx, g, qcur, qe, mbs[ntx], &ntx, &ndeq);

      ret = queue_dequeue(&qcur->dq);
      if (ret != 0)
      {
        LOG_ERROR("failed to dequeue queue for proto=%d", g->id);
        return -1;
      }
    }

    /* Rotate head if last polled queue didn't increment head */
    if (!qcur_empty)
    {
      qcur = &g->proto.dqueues[g->proto.dqueues_head];
      g->proto.dqueues[g->proto.dqueues_tail].next = qcur->id;
      g->proto.dqueues_tail = qcur->id;
      g->proto.dqueues_head = qcur->next;
      qcur->next = PROTOQ_ID_INVALID;
    }

    /* Subtract from guest's budget */
    tsc_spent = clock_rdtsc() - tsc_start;
    __atomic_fetch_sub(g->budget, tsc_spent, __ATOMIC_RELAXED);
  }

  fast_txflush(ctx);

  /* Free buffers that were not used */
  for (i = 0; i < max; i++)
    txcache_free(ctx, mbs[i]);

  return 0;
}

static inline void queues_poll_guest(struct fast_context *ctx,
    struct guest_fast *g, struct cham_dqueue *qcur, struct queue_entry *qe,
    struct rte_mbuf *mb, int *ntx, int *ndeq)
{
  int ret;
  int deq_ret;

  /* Prepare packet buffer for potential TX */
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

static inline void queues_poll_guest_comb(struct fast_context *ctx,
    struct guest_fast *g, struct cham_dqueue *qcur, struct queue_entry *qe,
    struct rte_mbuf *mb, int *ntx, int *ndeq)
{
  int deq_ret;
  struct fast_jit_deq_ctx jit_ctx = {
    .f_ctx = ctx,
    .g = g,
    .mb = mb,
    .qe = qe,
    .qid = qcur->id,
  };

  ebpf_vm_exec(g->proto.event_deq_vm, &jit_ctx, sizeof(jit_ctx), &deq_ret);
  (*ndeq)++;

  if (deq_ret > 0)
    (*ntx)++;
}
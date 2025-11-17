#include <stdlib.h>
#include <sys/mman.h>

#include <rte_malloc.h>

#include "nic_fast.h"
#include "fast.h"
#include "nic.h"
#include "nic_fast.h"
#include "queue_fns.h"
#include "queue_types.h"
#include "udp.h"
#include "log.h"
#include "config.h"
#include "controlif.h"
#include "ebpf.h"


struct guest_fast * init_guest(__u8 id, __u64 shm_len);
struct guest_fast * find_guest(struct fast_context *ctx, struct rte_mbuf *mbuf);

static inline void tx_cache_alloc(struct fast_context *ctx, 
    struct rte_mbuf ***mbs, __u16 num);
static inline void tx_cache_free(struct fast_context *ctx, struct rte_mbuf *mb);

int poll_rx(struct fast_context *ctx);
int poll_queues(struct fast_context *ctx);
int poll_tx(struct fast_context *ctx);
int poll_control(struct fast_context *ctx);
int tx_flush(struct fast_context *ctx);

int fast_context_init(struct fast_context *f_ctx, 
    struct nic_context *nic_ctx, __u16 thread_id,
    struct shm_handle *fc_handle, struct shm_handle *cf_handle,
    struct configuration *config, int shm_fd_internal, void *shm_base_internal)
{
  int i, j, ret;
  struct dqueue *cfq;
  struct equeue *fcq;
  struct guest_fast *guests;

  f_ctx->id = thread_id;
  f_ctx->shm_fd_internal = shm_fd_internal;
  f_ctx->shm_base_internal= shm_base_internal;
  nic_fast_init(nic_ctx, &f_ctx->nic_ctx, thread_id, config);

  cfq = dqueue_new(config->cham_queue_len, 
      sizeof(struct queue_entry),
      cf_handle->addr, cf_handle->off);
  if (cfq == NULL)
  {
    LOG_ERROR("failed to create fast to control path queue");
    return -1;
  }
  f_ctx->ctl_fast_q = cfq;

  fcq = equeue_new(config->cham_queue_len, sizeof(struct queue_entry),
      fc_handle->addr, fc_handle->off);
  if (fcq == NULL)
  {
    LOG_ERROR("failed to create control to fast path queue");
    return -1;
  }
  f_ctx->fast_ctl_q = fcq;

  guests = rte_calloc("fast path guests", config->max_guests, 
      sizeof(struct guest_fast), 0);
  if (guests == NULL)
  {
    LOG_ERROR("failed to allocate guest list");
    return -1;
  }
  f_ctx->guests = guests;

  /* Set initial ID to invalid for each scheduler entry in guest */
  for (i = 0; i < config->max_guests; i++)
  {
    for (j = 0; j < MAX_SCHED_ENTRIES; j++)
    {
      f_ctx->guests[i].proto.ebpf_ctx.sched.entries[j].id = SCHED_ID_INVALID;
    }
  }

  /* Preallocate mbufs so we don't have to do that in the critical path */
  ret = rte_pktmbuf_alloc_bulk(f_ctx->nic_ctx.pool, 
      f_ctx->tx_cache_mbs, TX_CACHE_SIZE);
  if (ret != 0)
  {
    LOG_ERROR("failed to preallocate tx cache");
    return -1;
  }
  f_ctx->tx_cache_n = TX_CACHE_SIZE;
  f_ctx->tx_cache_head = 0;

  return 0;
}

int fast_loop(struct fast_context *ctx)
{
  int ret;

  while(1) 
  {
    ret = poll_rx(ctx);
    if (ret < 0)
    {
      LOG_ERROR("poll_rx failed");
      return -1;
    }

    ret = poll_queues(ctx);
    if (ret < 0)
    {
      LOG_ERROR("poll_queues failed");
    }

    /* Flush entries in transmit buffer added by poll_queues */
    tx_flush(ctx);

    ret = poll_tx(ctx);
    if (ret < 0)
    {
      LOG_ERROR("poll_tx failed");
    }

    /* Flush entries in transmit buffer added by poll_tx */
    tx_flush(ctx);

    ret = poll_control(ctx);
    if (ret < 0)
    {
      LOG_ERROR("poll_control failed");
    }
  }
}

int poll_rx(struct fast_context *ctx)
{
  int i, n, ret;
  struct rte_mbuf *mbs[BATCH_SIZE];
  struct guest_fast *g;

  n = BATCH_SIZE;
  if (TXBUF_SIZE - ctx->tx_n < n)
    n = TXBUF_SIZE - ctx->tx_n;

  /* Receive packets from the NIC */
  n = nic_fast_rx(&ctx->nic_ctx, n, mbs);

  if (n <= 0)
    return 0;

  for (i = 0; i < n; i++)
  {
    g = find_guest(ctx, mbs[i]);
    if (g != NULL)
    {
      // g->proto.event_rx(rte_pktmbuf_mtod(mbs[i], __u8 *), 
          // &g->proto.handle);
      g->proto.ebpf_ctx.pkt = rte_pktmbuf_mtod(mbs[i], __u8 *);
      ebpf_vm_exec(g->proto.event_rx_vm, &g->proto.ebpf_ctx, 
          sizeof(struct cham_ebpf_ctx), &ret);
    }
  }

  rte_pktmbuf_free_bulk(mbs, n);

  return n;
}

int poll_queues(struct fast_context *ctx)
{
  int i, n, ret, deq_ret, ndeq, ntx;
  __u16 qid;
  struct guest_fast *g;
  struct cham_dqueue *q;
  struct queue_entry *qe;
  struct rte_mbuf **mbs;

  n = BATCH_SIZE;
  if (TXBUF_SIZE - ctx->tx_n < n)
    n = TXBUF_SIZE - ctx->tx_n;

  /* Allocate mbufs in case a message wants to transmit something already */
  tx_cache_alloc(ctx, &mbs, n);

  ntx = ndeq = 0;
  for (i = 0; i < ctx->n_guests && ndeq < n; i++)
  {
    g = &ctx->guests[i];
    qid = g->proto.dqueues_head;
    while (qid != PROTOQ_ID_INVALID && ndeq < n)
    {
      q = &g->proto.dqueues[qid];
        
      /* If there are no messages in queue continue */
      qe = queue_head(&q->dq);
      if (qe == NULL)
      {
        qid = q->next;
        continue;
      }

      /* Prepare packet */
      mbs[ntx]->data_off = 0;
      g->proto.ebpf_ctx.pkt = rte_pktmbuf_mtod(mbs[ntx], __u8 *);

      /* Add queue entry to ebpf context */
      g->proto.ebpf_ctx.qe = qe;
      g->proto.ebpf_ctx.qid = q->id;

      /* Execute custom dequeue procedure */
      ebpf_vm_exec(g->proto.event_deq_vm, &g->proto.ebpf_ctx, 
        sizeof(struct cham_ebpf_ctx), &deq_ret);
      ndeq++;

      /* Add to transmission buffer if packet processed for TX */
      if (deq_ret > 0)
      {
        mbs[ntx]->pkt_len = mbs[ntx]->data_len = deq_ret;
        ctx->tx_mbs[ctx->tx_n] = mbs[ntx];
        ctx->tx_n++;
        ntx++;
      }
      
      // g->proto.event_deq(q->id, qe, &g->proto.handle);

      /* Pop the queue */
      ret = queue_dequeue(&q->dq);
      if (ret != 0)
      {
        LOG_ERROR("failed to dequeue queue for proto=%d", g->id);
        return -1;
      }

      qid = q->next;
    }
  }

  /* Free buffers that were not used */
  for (i = ntx; i < n; i++)
    tx_cache_free(ctx, mbs[i]);

  return 0;
}

int poll_tx(struct fast_context *ctx)
{
  unsigned n;
  int i, tx_ret, ntx;
  struct guest_fast *g;
  struct rte_mbuf **mbs;
  __u8 n_guests = ctx->n_guests;
  
  if (ctx->guests == NULL)
    return 0;

  n = BATCH_SIZE;
  if (TXBUF_SIZE - ctx->tx_n < n)
    n = TXBUF_SIZE - ctx->tx_n;

  /* Allocate mbufs to use for transmission from mempool cache */
  tx_cache_alloc(ctx, &mbs, n);

  g = ctx->guests;
  ntx = 0;
  for (i = 0; i < n_guests && g != NULL && ntx < n; i++)
  {
    g = &ctx->guests[i];
    if (g->proto.event_tx_vm == NULL)
      continue;
    
    for (;ntx < n;)
    {
      /* Prepare packet */
      mbs[ntx]->data_off = 0;
      g->proto.ebpf_ctx.pkt = rte_pktmbuf_mtod(mbs[ntx], __u8 *);
      // ret = g->proto.event_tx(rte_pktmbuf_mtod(mbs[n_used], __u8 *), 
          // &guest->proto.handle);
      
      /* Execute custom TX procedure */
      ebpf_vm_exec(g->proto.event_tx_vm, &g->proto.ebpf_ctx.pkt, 
          sizeof(struct cham_ebpf_ctx), &tx_ret);

      /* Add to transmission buffer if packet processed for TX */
      if (tx_ret >= 0)
      {
        mbs[ntx]->pkt_len = mbs[ntx]->data_len = tx_ret;
        ctx->tx_mbs[ctx->tx_n] = mbs[ntx];
        ctx->tx_n++;
        ntx++;
      }
      else
      {
        break;
      }
    }
  }

  /* Free buffers that were not used */
  for (i = ntx; i < n; i++)
    tx_cache_free(ctx, mbs[i]);

  return 0;
}

int tx_flush(struct fast_context *ctx)
{
  int i, ret;

  /* Push packets to the NIC */
  ret = nic_fast_tx(&ctx->nic_ctx, ctx->tx_n, ctx->tx_mbs);

  if (ret == ctx->tx_n)
  {
    /* Everything sent */
    ctx->tx_n = 0;
  }
  else if (ret > 0)
  {
    /* Move unsent packets to front */
    for (i = ret; i < ctx->tx_n; i++)
      ctx->tx_mbs[i - ret] = ctx->tx_mbs[i];

    ctx->tx_n -= ret;
  }

  return ret;
}

int poll_control(struct fast_context *ctx)
{
  return controlif_poll(ctx);
}

struct guest_fast *find_guest(struct fast_context *ctx, struct rte_mbuf *mbuf)
{
  /* TODO: Use GRE headers to identify guest and protocol */
  return &ctx->guests[0];
}

static inline void tx_cache_alloc(struct fast_context *ctx, 
    struct rte_mbuf ***mbs, __u16 num)
{
  __u16 grow, tail, g;

  /* We don't have enough mbufs in the cache so allocate more */
  if (ctx->tx_cache_n < num)
  {
    grow = TX_CACHE_SIZE - ctx->tx_cache_n;
    tail = (ctx->tx_cache_head + ctx->tx_cache_n) & (TX_CACHE_SIZE - 1);

    if (tail + grow <= TX_CACHE_SIZE)
    {
      assert(rte_pktmbuf_alloc_bulk(ctx->nic_ctx.pool, 
          ctx->tx_cache_mbs + tail , grow) == 0);
    }
    else
    {
      g = TX_CACHE_SIZE - tail;
      assert(rte_pktmbuf_alloc_bulk(ctx->nic_ctx.pool, 
          ctx->tx_cache_mbs + tail, g) == 0);
      assert(rte_pktmbuf_alloc_bulk(ctx->nic_ctx.pool, 
          ctx->tx_cache_mbs, grow - g) == 0);
    }

    ctx->tx_cache_n += grow;
  }

  *mbs = ctx->tx_cache_mbs + ctx->tx_cache_head;

  ctx->tx_cache_head = (ctx->tx_cache_head + num) & (TX_CACHE_SIZE - 1);
  ctx->tx_cache_n -= num;
}

static inline void tx_cache_free(struct fast_context *ctx, struct rte_mbuf *mb)
{
  __u16 n, head;

  n = ctx->tx_cache_n;
  if (n < TX_CACHE_SIZE)
  {
    /* Return mbuf to the cache */
    head = (ctx->tx_cache_head + n) & (TX_CACHE_SIZE - 1);
    ctx->tx_cache_mbs[head] = mb;
    ctx->tx_cache_n = n + 1;
    mb->ol_flags = 0;
  }
  else
  {
    /* The cache is full so return to the DPDK mempool */
    rte_pktmbuf_free(mb);
    mb->ol_flags = 0;
  }
}


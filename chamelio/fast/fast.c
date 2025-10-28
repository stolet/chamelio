#include <stdlib.h>
#include <sys/mman.h>

#include <rte_malloc.h>

#include "nic_fast.h"
#include "fast.h"
#include "nic.h"
#include "nic_fast.h"
#include "queue.h"
#include "udp.h"
#include "log.h"
#include "config.h"
#include "controlif.h"
#include "cham_scheduler.h"


struct guest_fast * init_guest(uint8_t id, uint64_t shm_len);
struct guest_fast *find_guest(struct fast_context *ctx, struct rte_mbuf *mbuf);

static inline void tx_cache_alloc(struct fast_context *ctx, 
    struct rte_mbuf ***mbs, uint16_t num);
static inline void tx_cache_free(struct fast_context *ctx, struct rte_mbuf *mb);

int poll_rx(struct fast_context *ctx);
int poll_queues(struct fast_context *ctx);
int poll_tx(struct fast_context *ctx);
int poll_control(struct fast_context *ctx);

int fast_context_init(struct fast_context *f_ctx, 
    struct nic_context *nic_ctx, uint16_t thread_id,
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
      f_ctx->guests[i].proto.handle.sched.entries[j].id = SCHED_ID_INVALID;
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

    ret = poll_tx(ctx);
    if (ret < 0)
    {
      LOG_ERROR("poll_tx failed");
    }

    ret = poll_control(ctx);
    if (ret < 0)
    {
      LOG_ERROR("poll_control failed");
    }
  }
}

int poll_rx(struct fast_context *ctx)
{
  int i, n;
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
      g->proto.event_rx(rte_pktmbuf_mtod(mbs[i], uint8_t *), 
          &g->proto.handle);
    }
  }

  rte_pktmbuf_free_bulk(mbs, n);

  return n;
}

int poll_queues(struct fast_context *ctx)
{
  int i, ret, ndeq;
  uint16_t qid;
  struct guest_fast *g;
  struct cham_dqueue *q;
  struct queue_entry *qe;

  ndeq = 0;
  for (i = 0; i < ctx->n_guests && ndeq < BATCH_SIZE; i++)
  {
    g = &ctx->guests[i];
    qid = g->proto.dqueues_head;
    while (qid != PROTOQ_ID_INVALID && ndeq < BATCH_SIZE)
    {
      q = &g->proto.dqueues[qid];
        
      /* If there are no messages in queue continue */
      qe = queue_head(&q->dq);
      if (qe == NULL)
      {
        qid = q->next;
        continue;
      }

      /* Execute custom dequeue procedure */
      ndeq++;
      g->proto.event_deq(q->id, qe, &g->proto.handle);

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
  return 0;
}

int poll_tx(struct fast_context *ctx)
{
  unsigned n;
  int i, ret, n_used;
  struct guest_fast *guest;
  struct rte_mbuf **mbs;
  uint8_t n_guests = ctx->n_guests;
  
  if (ctx->guests == NULL)
    return 0;

  n = BATCH_SIZE;
  if (TXBUF_SIZE - ctx->tx_n < n)
    n = TXBUF_SIZE - ctx->tx_n;

  /* Allocate mbufs to use for transmission from mempool cache */
  tx_cache_alloc(ctx, &mbs, n);

  guest = ctx->guests;
  n_used = 0;
  for (i = 0; i < n_guests && guest != NULL && n_used < n; i++)
  {
    for (;n_used < n;)
    {
      mbs[n_used]->data_off = 0;
      ret = guest->proto.event_tx(rte_pktmbuf_mtod(mbs[n_used], uint8_t *), 
          &guest->proto.handle);

      if (ret >= 0)
      {
        mbs[n_used]->pkt_len = mbs[n_used]->data_len = ret;
        ctx->tx_mbs[ctx->tx_n] = mbs[n_used];
        ctx->tx_n++;
        n_used++;
      }
      else
      {
        break;
      }
    }
  }

  /* Push packets to the NIC */
  ret = nic_fast_tx(&ctx->nic_ctx, ctx->tx_n, ctx->tx_mbs);
  
  /* Free buffers that were not used */
  for (i = n_used; i < n; i++)
    tx_cache_free(ctx, mbs[i]);

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

  return 0;
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
    struct rte_mbuf ***mbs, uint16_t num)
{
  uint16_t grow, tail, g;

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
  uint16_t n, head;

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


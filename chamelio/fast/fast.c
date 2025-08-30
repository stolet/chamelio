#include <stdlib.h>
#include <sys/mman.h>

#include <rte_malloc.h>

#include "nic_fast.h"
#include "fast_process.h"
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

int poll_rx(struct fast_context *ctx);
int poll_queues(struct fast_context *ctx);
int poll_tx(struct fast_context *ctx);
int poll_control(struct fast_context *ctx);

int fast_context_init(struct fast_context *f_ctx, 
    struct nic_context *nic_ctx, uint16_t thread_id,
    struct shm_handle *fc_handle, struct shm_handle *cf_handle,
    struct configuration *config, int shm_fd_internal, void *shm_base_internal)
{
  int i, j;
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

  return 0;
}

void fast_context_destroy()
{
  /* TODO: cleanup fast path context */
}

int fast_loop(struct fast_context *ctx)
{
  int ret;

  while(1) 
  {
    // ret = poll_rx(ctx);
    // if (ret < 0)
    // {
    //   LOG_ERROR("poll_rx failed");
    //   return -1;
    // }

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
  uint8_t rx_err;
  struct rte_mbuf *mbs[BATCH_SIZE];

  n = BATCH_SIZE;
  if (TXBUF_SIZE - ctx->tx_n < n)
    n = TXBUF_SIZE - ctx->tx_n;

  /* Receive packets from the NIC */
  n = nic_fast_rx(&ctx->nic_ctx, n, mbs);

  if (n <= 0)
    return 0;

  for (i = 0; i < n; i++)
  {
    rx_err = fast_process_packet_rx(ctx, mbs[i]);
    if (rx_err != 0)
      LOG_ERROR("fast_process_packet_rx failed");
  }

  rte_pktmbuf_free_bulk(mbs, n);

  return n;
}

// int poll_sched(struct fast_context *ctx, struct cham_ready_entry *ready_entries)
// {
//   int i, nsched;
//   struct guest_fast *g;
//   struct cham_scheduler *sched;
//   struct cham_ready_entry *re;
//   struct cham_sched_entry *se;

//   nsched = 0;
//   for (i = 0; i < ctx->n_guests && nsched < BATCH_SIZE; i++)
//   {
//     g = &ctx->guests[i];
//     sched = &g->proto.handle.sched;
//     re = &ready_entries[nsched];
//     se = sched_head(sched);

//     /* This protocol has nothing to schedule */
//     if (se == NULL)
//       continue;

//     /* Run the custom transmit scheduler for this protocol */
//     g->proto.act_txsched(se, re);

//     /* Protocol actually staged something to the ready queue */
//     if (re->id != SCHED_ID_INVALID)
//     {
//       sched_pop(sched);
//       nsched++;
//     }
//   }

//   return 0;
// }

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
  int i, ret;
  struct guest_fast *guest;
  struct rte_mbuf *mbs[BATCH_SIZE];
  uint8_t n_guests = ctx->n_guests;
  
  if (ctx->guests == NULL)
    return 0;

  n = BATCH_SIZE;
  if (TXBUF_SIZE - ctx->tx_n < n)
    n = TXBUF_SIZE - ctx->tx_n;

  /* Allocate mbufs to use for transmission */
  /* TODO: Have a cache for the mempool, 
     free only what we used and don't allocate every loop */
  ret = rte_pktmbuf_alloc_bulk(ctx->nic_ctx.pool, mbs, n);
  if (ret < 0)
  {
    LOG_ERROR("not enough entries in the mempool");
    abort();
    return -1;
  }

  guest = ctx->guests;
  for (i = 0; i < n_guests && guest != NULL && i < n; i++)
  {
    ret = guest->proto.event_tx(mbs[i]->buf_addr, &guest->proto.handle);
    if (ret == 0)
    {
      ctx->tx_mbs[ctx->tx_n] = mbs[i];
      ctx->tx_n++;
    }
  }

  /* Push packets to the NIC */
  ret = nic_fast_tx(&ctx->nic_ctx, ctx->tx_n, ctx->tx_mbs);
  rte_pktmbuf_free_bulk(mbs, n);

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
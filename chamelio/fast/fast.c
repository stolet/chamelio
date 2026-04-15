#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <rte_malloc.h>

#include "nic.h"
#include "nic_fast.h"
#include "fast.h"
#include "queue_fns.h"
#include "queue_types.h"
#include "log.h"
#include "config.h"
#include "controlif.h"
#include "fast_rx.h"
#include "fast_tx.h"
#include "fast_queues.h"


struct guest_fast * init_guest(__u8 id, __u64 shm_len);
int fast_loop_default(struct fast_context *ctx);
int fast_loop_comb(struct fast_context *ctx);
static inline int agg_fns_ready(struct fast_context *ctx);


int fast_context_init(struct fast_context *f_ctx, 
    struct nic_context *nic_ctx, __u16 thread_id,
    struct shm_handle *fc_handle, struct shm_handle *cf_handle, 
    struct shm_handle *ctxq_handle, struct configuration *config, 
    int shm_fd_internal, void *shm_base_internal)
{
  int i, j, ret;
  struct dqueue *cfq, *ctxq;
  struct equeue *fcq;

  f_ctx->id = thread_id;
  f_ctx->config = config;
  f_ctx->fp_jit_combined = config->fp_jit_combined;
  f_ctx->fp_proto_mode = config->fp_proto_mode;
  f_ctx->virt_gre = config->virt_gre;
  f_ctx->perf_iso = config->perf_iso;
  f_ctx->agg_rx_fn = NULL;
  f_ctx->agg_deq_fn = NULL;
  f_ctx->agg_tx_fn = NULL;
  f_ctx->shm_fd_internal = shm_fd_internal;
  f_ctx->shm_base_internal= shm_base_internal;
  nic_fast_init(nic_ctx, &f_ctx->nic_ctx, thread_id, config);

  /* Initialize ARP table with default values */
  arp_table_init(&f_ctx->arp_table);
  
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
  
  ctxq = dqueue_new(config->control_txq_len, config->control_txq_pkt_len,
      ctxq_handle->addr, ctxq_handle->off);
  if (ctxq == NULL)
  {
    LOG_ERROR("failed to create control tx queue");
    return -1;
  }
  f_ctx->ctl_txq = ctxq;

  f_ctx->n_guests = 0;

  /* Set initial ID to invalid for each scheduler entry in guest */
  for (i = 0; i < CHAMELIO_MAX_GUESTS; i++)
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

int fast_batch_stats_snapshot(const struct fast_context *ctx,
    struct fast_batch_counters *stats)
{
  if (ctx == NULL || stats == NULL)
    return -1;

  *stats = ctx->batch_stats;
  return 0;
}

int fast_loop(struct fast_context *ctx)
{
  int ret;
  
  if (ctx->fp_jit_combined)
    ret = fast_loop_comb(ctx);
  else
    ret = fast_loop_default(ctx);
    
  return ret;
}

int fast_loop_default(struct fast_context *ctx)
{
  int ret;
  
  while (1)
  {
    ret = fast_rx_poll(ctx);
    if (ret < 0)
    {
      LOG_ERROR("poll_rx failed");
      return -1;
    }
    ctx->batch_stats.rx_calls++;
    ctx->batch_stats.rx_items += ret;

    ret = fast_queues_poll(ctx);
    if (ret < 0)
    {
      LOG_ERROR("poll_queues failed");
    }
    else
    {
      ctx->batch_stats.queue_calls++;
      ctx->batch_stats.queue_items += ret;
    }

    ret = fast_tx_poll(ctx);
    if (ret < 0)
    {
      LOG_ERROR("poll_tx failed");
    }
    else
    {
      ctx->batch_stats.tx_calls++;
      ctx->batch_stats.tx_items += ret;
    }

    ret = controlif_poll(ctx);
    if (ret < 0)
    {
      LOG_ERROR("controlif_poll failed");
    }
    
    /* Flush entries in transmit buffer added by controlif_poll */
    fast_txflush(ctx);
  }
}

int fast_loop_comb(struct fast_context *ctx)
{
  int ret;
  
  while (1)
  {
    if (!agg_fns_ready(ctx))
    {
      ret = controlif_poll(ctx);
      if (ret < 0)
      {
        LOG_ERROR("controlif_poll failed");
        return -1;
      }
      continue;
    }

    ret = (int) ctx->agg_rx_fn(ctx, sizeof(*ctx));
    if (ret < 0)
    {
      LOG_ERROR("poll_rx_comb failed");
      return -1;
    }
    ctx->batch_stats.rx_calls++;
    ctx->batch_stats.rx_items += ret;

    ret = (int) ctx->agg_deq_fn(ctx, sizeof(*ctx));
    if (ret < 0)
    {
      LOG_ERROR("poll_queues_comb failed");
    }
    else
    {
      ctx->batch_stats.queue_calls++;
      ctx->batch_stats.queue_items += ret;
    }

    ret = (int) ctx->agg_tx_fn(ctx, sizeof(*ctx));
    if (ret < 0)
    {
      LOG_ERROR("poll_tx_comb failed");
    }
    else
    {
      ctx->batch_stats.tx_calls++;
      ctx->batch_stats.tx_items += ret;
    }

    ret = controlif_poll(ctx);
    if (ret < 0)
    {
      LOG_ERROR("controlif_poll failed");
    }
    
    /* Flush entries in transmit buffer added by controlif_poll */
    fast_txflush(ctx);
  }
}

static inline int agg_fns_ready(struct fast_context *ctx)
{
  return ctx->agg_rx_fn != NULL &&
      ctx->agg_deq_fn != NULL &&
      ctx->agg_tx_fn != NULL;
}

int fast_txflush(struct fast_context *ctx)
{
  int ret, unsent;

  if (ctx->tx_n == 0)
    return 0;
  
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
    unsent = ctx->tx_n - ret;
    memmove(ctx->tx_mbs, ctx->tx_mbs + ret, unsent * sizeof(ctx->tx_mbs[0]));
    ctx->tx_n = unsent;
  }

  return ret;
}

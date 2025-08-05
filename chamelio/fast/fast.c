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
#include "slowif.h"


struct guest_fast * init_guest(uint8_t id, uint64_t shm_len);
struct app_fast * init_app(uint8_t id);

int poll_rx(struct fast_context *ctx);
int poll_queues(struct fast_context *ctx);
int poll_tx(struct fast_context *ctx);
int poll_slow(struct fast_context *ctx);

int fast_context_init(struct fast_context *f_ctx, 
    struct nic_context *nic_ctx, uint16_t thread_id,
    struct shm_handle *fs_handle, struct shm_handle *sf_handle,
    struct configuration *config, int shm_fd_internal, void *shm_base_internal)
{
  int i, j, n_apps, n_app_ctxs;
  struct dqueue *sfq;
  struct equeue *fsq;
  struct guest_fast *guests;
  struct app_fast *apps;
  struct app_context_fast *app_ctxs;

  f_ctx->id = thread_id;
  f_ctx->shm_fd_internal = shm_fd_internal;
  f_ctx->shm_base_internal= shm_base_internal;
  nic_fast_init(nic_ctx, &f_ctx->nic_ctx, thread_id, config);

  sfq = dqueue_new(config->cham_queue_len, sf_handle->addr, sf_handle->off);
  if (sfq == NULL)
  {
    LOG_ERROR("failed to create fast to slow path queue");
    return -1;
  }
  f_ctx->slow_fast_q = sfq;

  fsq = equeue_new(config->cham_queue_len, fs_handle->addr, fs_handle->off);
  if (fsq == NULL)
  {
    LOG_ERROR("failed to create slow to fast path queue");
    return -1;
  }
  f_ctx->fast_slow_q = fsq;

  guests = rte_calloc("fast path guests", config->max_guests, 
      sizeof(struct guest_fast), 0);
  if (guests == NULL)
  {
    LOG_ERROR("failed to allocate guest list");
    return -1;
  }
  f_ctx->guests = guests;

  for (i = 0; i < config->max_guests; i++)
  {
    apps = rte_calloc("fast path apps", config->max_apps, 
        sizeof(struct app_fast), 0);
    if (apps == NULL)
    {
      LOG_ERROR("failed to allocate app list");
      goto free_apps;
    }
    f_ctx->guests[i].apps = apps;
    n_apps++;

    for (j = 0; j < config->max_apps; j++)
    {
      app_ctxs = rte_calloc("fast path app ctxs", config->max_app_ctxs, 
          sizeof(struct app_context_fast), 0);

      if (app_ctxs == NULL)
      {
        LOG_ERROR("failed to allocate app context list");
        goto free_app_ctxs;
      }
      f_ctx->guests[i].apps[j].app_ctxs = app_ctxs;
      n_app_ctxs++;
    }
  }

  return 0;

free_app_ctxs:
  for (i = 0; i < n_apps; i++)
    for (j = 0; j < n_app_ctxs; j++)
      free(f_ctx->guests[i].apps[j].app_ctxs);
free_apps:
  for (i = 0; i < n_apps; i++)
    free(f_ctx->guests[i].apps);

  free(guests);
  return -1;
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

    ret = poll_slow(ctx);
    if (ret < 0)
    {
      LOG_ERROR("poll_slow failed");
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
  {
    return 0;
  }

  for (i = 0; i < n; i++)
  {
    rx_err = fast_process_packet_rx(ctx, mbs[i]);
    if (rx_err != 0)
    {
      LOG_ERROR("fast_process_packet_rx failed");
      fast_process_packet_error(ctx, mbs[i], rx_err);
    }
  }

  rte_pktmbuf_free_bulk(mbs, n);

  return n;
}

int poll_queues(struct fast_context *ctx)
{
  return 0;
}

int poll_tx(struct fast_context *ctx)
{
  unsigned n;
  int i, ret;
  uint8_t tx_err;
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
    tx_err = fast_process_packet_tx(ctx, mbs[i]);
    if (tx_err < 0)
    {
      LOG_WARN("fast_process_packet_tx failed sending to slow path");
      fast_process_packet_error(ctx, mbs[i], tx_err);
    }
    else
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
    {
      ctx->tx_mbs[i - ret] = ctx->tx_mbs[i];
    }
    ctx->tx_n -= ret;
  }

  return 0;
}

int poll_slow(struct fast_context *ctx)
{
  return slowif_poll(ctx);
}
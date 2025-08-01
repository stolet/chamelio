#include <stdlib.h>
#include <stdint.h>
#include <assert.h>

#include "slow.h"
#include "guestif.h"
#include "appif.h"
#include "config.h"
#include "log.h"
#include "queue.h"

int poll_fast();

int slow_context_init(struct slow_context *ctx, struct configuration *config,
    struct shm_handle **fs_handles, struct shm_handle **sf_handles)
{
  int i;
  struct equeue *sfq;
  struct dqueue *fsq;
  struct dqueue **fast_slow_qs;
  struct equeue **slow_fast_qs;

  ctx->config = config;
  
  ctx->guest_uxfd = -1;
  ctx->guest_epfd = -1;
  ctx->guest_id_next = 0;
  
  ctx->app_uxfd = -1;
  ctx->app_epfd = -1;
  ctx->app_id_next = 0;

  /* Allocate pointer list for queues */
  fast_slow_qs = malloc(sizeof(struct dqueue *) * config->fp_cores_max);
  if (fast_slow_qs == NULL)
  {
    LOG_ERROR("failed to allocate list of fast->slow queues");
    return -1;
  }
  ctx->fast_slow_qs = fast_slow_qs;

  slow_fast_qs = malloc(sizeof(struct equeue *) * config->fp_cores_max);
  if (slow_fast_qs == NULL)
  {
    LOG_ERROR("failed to alloacate list of slow->fast queues");
    goto free_fast_slow_list;
  }
  ctx->slow_fast_qs = slow_fast_qs;

  /* Create a queue with each shared memory handle */
  for (i = 0; i < config->fp_cores_max; i++)
  {
    sfq = equeue_new(config->cham_queue_len, 
        sf_handles[i]->addr, sf_handles[i]->off);
    if (sfq == NULL)
    {
      LOG_ERROR("failed to create fast to slow path queue");
      goto free_slow_fast_list;
    }
    ctx->slow_fast_qs[i] = sfq;

    fsq = dqueue_new(config->cham_queue_len, 
        fs_handles[i]->addr, fs_handles[i]->off);
    if (fsq == NULL)
    {
      LOG_ERROR("failed to create slow to fast path queue");
      goto free_slow_fast_list;
    }
    ctx->fast_slow_qs[i] = fsq;
  }

  return 0;

free_slow_fast_list:
  free(slow_fast_qs);
free_fast_slow_list:
  free(fast_slow_qs);
  return -1;
}

int slow_loop(struct slow_context *ctx)
{
  int ret;

  ret = guestif_init(ctx);
  if (ret != 0)
  {
    LOG_ERROR("failed to initialise guestif");
    return -1;
  }

  ret = appif_init(ctx);
  if (ret != 0)
  {
    LOG_ERROR("failed to initialise appif");
    return -1;
  }

  while (1) 
  {
    guestif_poll(ctx);
    appif_poll(ctx);
    poll_fast(ctx);
  }
}

int poll_fast(struct slow_context *ctx)
{
  uint8_t type;
  struct dqueue *q;
  struct queue_entry *qe;
  int i;

  for (i = 0; i < ctx->config->fp_cores_max; i++)
  {
    /* TODO: Dequeue in batches to improve performance and prevent us
       from spending too much time in one core */
    q = ctx->fast_slow_qs[i];
    qe = queue_head(q);

    if (qe != NULL)
    {
      type = qe->type;
      switch (type)
      {
        case QUEUE_EMPTY:
          break;
        case QUEUE_ARP_TX:
          break;
        case QUEUE_ARP_RX:
          break;
        default:
          LOG_ERROR("unknown queue entry type from "
              "fast path to slow path type=%d", type);
          abort();
      }
    }
  }

  return 0;
}
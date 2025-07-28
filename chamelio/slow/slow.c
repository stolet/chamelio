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

  ctx->config = config;
  
  ctx->guest_uxfd = -1;
  ctx->guest_epfd = -1;
  ctx->guest_id_next = 0;
  
  ctx->app_uxfd = -1;
  ctx->app_epfd = -1;
  ctx->app_id_next = 0;

  /* Create a queue with each shared memory handle */
  for (i = 0; i < config->fp_cores_max; i++)
  {
    /* TODO: Pass len as a queue parameter */
    sfq = equeue_new(sf_handles[i]->len, sf_handles[i]);
    if (sfq == NULL)
    {
      LOG_ERROR("failed to create fast to slow path queue");
      return -1;
    }
    ctx->slow_fast_qs[i] = sfq;

    fsq = dqueue_new(fs_handles[i]->len, sf_handles[i]);
    if (fsq == NULL)
    {
      LOG_ERROR("failed to create slow to fast path queue");
      return -1;
    }
    ctx->fast_slow_qs[i] = fsq;
  }

  return 0;
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
        case QUEUE_ARP_TX:
          LOG_DEBUG("sending arp tx to slow");
          break;
        case QUEUE_ARP_RX:
          LOG_DEBUG("sending arp rx to slow");
          break;
        default:
          LOG_ERROR("unknown queue entry type from fast path to slow path");
          abort();
      }

      assert(queue_dequeue(q) == 0);
    }
  }

  return 0;
}
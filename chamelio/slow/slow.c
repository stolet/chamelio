#include <stdlib.h>
#include <stdint.h>
#include <assert.h>

#include "slow.h"
#include "guestif.h"
#include "config.h"
#include "log.h"
#include "queue.h"

int poll_fast();

int slow_context_init(struct slow_context *ctx, struct configuration *config,
    struct queue **fast_slow_qs, struct queue **slow_fast_qs)
{
  ctx->config = config;
  ctx->fast_slow_qs = fast_slow_qs;
  ctx->slow_fast_qs = slow_fast_qs;

  ctx->guest_uxfd = -1;
  ctx->guest_epfd = -1;
  ctx->guest_id_next = 0;

  ctx->app_uxfd = -1;
  ctx->app_epfd = -1;
  ctx->app_id_next = 0;

  return 0;
}

int slow_loop(struct slow_context *ctx)
{
  int ret;

  ret = guestif_init(ctx);
  if (ret != 0)
  {
    LOG_ERROR("failed to initialize guestif");
    return -1;
  }

  while (1) 
  {
    guestif_poll(ctx);
    poll_fast(ctx);
  }
}

int poll_fast(struct slow_context *ctx)
{
  uint8_t type;
  struct queue *q;
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
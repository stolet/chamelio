#include <stdlib.h>
#include <stdint.h>
#include <assert.h>

#include "slow.h"
#include "../queue/queue.h"
#include "../utils/log/log.h"

int poll_fast();

int slow_context_init(struct slow_context *ctx, uint16_t n_cores,
    struct queue **fast_slow_qs, struct queue **slow_fast_qs)
{
  ctx->n_cores = n_cores;
  ctx->fast_slow_qs = fast_slow_qs;
  ctx->slow_fast_qs = slow_fast_qs;

  return 0;
}

int slow_loop(struct slow_context *ctx)
{
  while (1) 
  {
    poll_fast(ctx);
  }
}

int poll_fast(struct slow_context *ctx)
{
  uint8_t type;
  struct queue *q;
  struct queue_entry *qe;
  int i;

  for (i = 0; i < ctx->n_cores; i++)
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
        case QUEUE_TYPE_ARP_TX:
          LOG_DEBUG("sending arp tx to slow");
          break;
        case QUEUE_TYPE_ARP_RX:
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
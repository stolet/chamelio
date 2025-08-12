#include "fast.h"
#include "queue.h"
#include "udp.h"

static int handle_new_guest(struct fast_context *ctx, struct queue_entry *qe);

int controlif_poll(struct fast_context *ctx)
{
  uint8_t type;
  struct dqueue *q;
  struct queue_entry *qe;

  /* TODO: Poll up to batch size */
  q = ctx->ctl_fast_q;
  qe = queue_head(q);

  if (qe == NULL)
    return 0;
  
  type = qe->type;
  switch (type)
  {
    case QUEUE_EMPTY:
      break;
    case QUEUE_NEW_GUEST:
      handle_new_guest(ctx, qe);
      queue_dequeue(q);
      break;
    default:
      LOG_WARN("unknown queue tryt type from control path" 
          "to fast path type=%d", type);
      break;
  }

  return 0;
}

static int handle_new_guest(struct fast_context *ctx, struct queue_entry *qe)
{
  struct guest_fast *g;
  struct queue_new_guest_req *req = (struct queue_new_guest_req *) &qe->data;

  g = &ctx->guests[req->id];
  g->id = req->id;
  
  g->shm_base = req->shm_base;
  g->shm_len = req->shm_len;
  
  ctx->n_guests++;

  return 0;
}
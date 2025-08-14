#include "fast.h"
#include "queue.h"
#include "udp.h"

static void handle_new_guest(struct fast_context *ctx, struct queue_entry *qe);
static void handle_new_queues(struct fast_context *ctx, struct queue_entry *qe);
static void handle_new_map(struct fast_context *ctx, struct queue_entry *qe);
static void handle_enableq(struct fast_context *ctx, struct queue_entry *qe);
static void handle_disableq(struct fast_context *ctx, struct queue_entry *qe);

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
    case QUEUE_NEW_GUEST_REQ:
      handle_new_guest(ctx, qe);
      queue_dequeue(q);
      break;
    case QUEUE_NEW_QUEUES_REQ:
      handle_new_queues(ctx, qe);
      queue_dequeue(q);
      break;
    case QUEUE_NEW_MAP_REQ:
      handle_new_map(ctx, qe);
      queue_dequeue(q);
      break;
    case QUEUE_ENABLEQ_REQ:
      handle_enableq(ctx, qe);
      queue_dequeue(q);
      break;
    case QUEUE_DISABLEQ_REQ:
      handle_disableq(ctx, qe);
      queue_dequeue(q);
      break;
    default:
      LOG_WARN("unknown queue tryt type from control path" 
          "to fast path type=%d", type);
      break;
  }

  return 0;
}

static void handle_new_guest(struct fast_context *ctx, struct queue_entry *qe)
{
  struct guest_fast *g;
  struct queue_new_guest_req *req = (struct queue_new_guest_req *) &qe->data;

  g = &ctx->guests[req->id];
  g->id = req->id;
  
  g->shm_base = req->shm_base;
  g->shm_len = req->shm_len;
  
  ctx->n_guests++;
}

static void handle_new_queues(struct fast_context *ctx, struct queue_entry *qe)
{
  int i;
  struct guest_fast *g;
  struct proto_fast *p;
  struct proto_queue_fast *q;

  struct queue_new_queues_req *req = (struct queue_new_queues_req *) &qe->data;

  g = &ctx->guests[req->gid];
  p = &g->proto;
  p->nqueues = req->nqueues;
  p->nelems = req->nelems;
  p->elsize = req->elsize;

  for (i = 0; i < req->nqueues; i++)
  {
    q = &p->queues[i];
    q->id = i;
    q->core = 0;
    q->active = PROTOQ_DISABLED;
    q->off = req->offs[i];
    q->proto = p;
    q->size = req->elsize * req->nelems;
  }

  LOG_DEBUG("created queues in fast-path");
}

static void handle_new_map(struct fast_context *ctx, struct queue_entry *qe)
{
  struct guest_fast *g;
  struct proto_fast *p;
  struct proto_map_fast *m;
  
  struct queue_new_map_req *req = (struct queue_new_map_req *) &qe->data;

  g = &ctx->guests[req->gid];
  p = &g->proto;
  m = &p->maps[p->nmaps];

  m->id = p->nmaps;
  m->elsize = req->elsize;
  m->nelems = req->nelems;
  m->off = req->off;
  m->proto = p;

  p->nmaps++;
  LOG_DEBUG("created map in fast-path");
}

static void handle_enableq(struct fast_context *ctx, struct queue_entry *qe)
{
  struct guest_fast *g;
  struct proto_fast *p;
  struct queue_enableq_req *req;
  
  req = (struct queue_enableq_req *) &qe->data;
  g = &ctx->guests[req->gid];
  p = &g->proto;
  
  p->queues[req->qid].active = PROTOQ_ENABLED;
  LOG_DEBUG("enabled queue qid=%d in core=%d", req->qid, req->core);
}

static void handle_disableq(struct fast_context *ctx, struct queue_entry *qe)
{
  struct guest_fast *g;
  struct proto_fast *p;
  struct queue_disableq_req *req;
  
  req = (struct queue_disableq_req *) &qe->data;
  g = &ctx->guests[req->gid];
  p = &g->proto;
  
  p->queues[req->qid].active = PROTOQ_DISABLED;
  LOG_DEBUG("disabled queue qid=%d in core=%d", req->qid, req->core);
}
#include "fast.h"
#include "queue.h"
#include "udp_fast.h"
#include "cham_scheduler.h"

static void handle_new_guest(struct fast_context *ctx, struct queue_entry *qe);
static void handle_new_queue(struct fast_context *ctx, struct queue_entry *qe);
static void handle_new_map(struct fast_context *ctx, struct queue_entry *qe);
static void handle_enableq(struct fast_context *ctx, struct queue_entry *qe);
static void handle_disableq(struct fast_context *ctx, struct queue_entry *qe);

int controlif_poll(struct fast_context *ctx)
{
  int i;
  uint8_t type;
  struct dqueue *q;
  struct queue_entry *qe;
 
  q = ctx->ctl_fast_q;

  for (i = 0; i < BATCH_SIZE; i++)
  {
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
      case QUEUE_NEW_MAP_REQ:
        handle_new_map(ctx, qe);
        queue_dequeue(q);
        break;
      case QUEUE_NEW_QUEUE_REQ:
        handle_new_queue(ctx, qe);
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
  }

  return 0;
}

static void handle_new_guest(struct fast_context *ctx, struct queue_entry *qe)
{
  struct guest_fast *g;
  struct queue_new_guest_req *req = &qe->data.new_guest_req;

  g = &ctx->guests[req->id];
  g->id = req->id;
  g->shm_base = req->shm_base;
  g->shm_len = req->shm_len;
  
  /* TODO: Have a separate message to initialise protocol */
  g->proto.ndqueues = 0;
  g->proto.dqueues_head = PROTOQ_ID_INVALID;
  g->proto.dqueues_tail = PROTOQ_ID_INVALID;

  /* TODO: Add ebpf code here */
  g->proto.event_rx = udp_event_rx;
  g->proto.event_tx = udp_event_tx;
  g->proto.event_deq = udp_event_deq;
  
  /* Init qman */
  sched_init(&g->proto.handle.sched);
  g->proto.handle.shm_base = g->shm_base;

  ctx->n_guests++;
}

static void handle_new_queue(struct fast_context *ctx, struct queue_entry *qe)
{
  struct guest_fast *g;
  struct proto_fast *p;
  struct cham_equeue *q;
  
  struct queue_new_queue_req *req = &qe->data.new_queue_req;

  g = &ctx->guests[req->gid];
  p = &g->proto;
  q = &p->handle.equeues[req->qid];
  q->id = req->qid;

  equeue_init(&q->eq, req->nelems, req->elsize, g->shm_base + req->off, req->off);
  LOG_DEBUG("created queue qid=%d in fast-path", req->qid);
}

static void handle_new_map(struct fast_context *ctx, struct queue_entry *qe)
{
  struct guest_fast *g;
  struct proto_fast *p;
  struct cham_map *m;
  
  struct queue_new_map_req *req = &qe->data.new_map_req;

  g = &ctx->guests[req->gid];
  p = &g->proto;
  m = &p->handle.maps[req->gid];

  m->id = req->mid;
  m->elsize = req->elsize;
  m->nelems = req->nelems;
  m->off = req->off;
  m->addr = g->shm_base + req->off;
  LOG_DEBUG("created map qid=%d in fast-path", req->mid);
}

static void handle_enableq(struct fast_context *ctx, struct queue_entry *qe)
{
  struct guest_fast *g;
  struct proto_fast *p;
  struct queue_enableq_req *req;
  struct cham_dqueue *q;
  
  req = &qe->data.enableq_req;
  g = &ctx->guests[req->gid];
  p = &g->proto;
  
  /* Get uninitialised queue from protocol list */
  q = &p->dqueues[req->qid];
  q->id = req->qid;

  /* Initialise dequeue struct */
  q->dq.head = 0;
  q->dq.nelems = req->nelems;
  q->dq.elsize = req->elsize;
  q->dq.off = req->off;
  q->dq.entries = g->shm_base + req->off;
  
  /* Add queue to protocol list */
  if (p->dqueues_tail == PROTOQ_ID_INVALID)
    p->dqueues_head = req->qid;
  else
    p->dqueues[p->dqueues_tail].next = req->qid;

  q->prev = p->dqueues_tail;
  q->next = PROTOQ_ID_INVALID;
  p->dqueues_tail = req->qid;
  p->ndqueues++;
  
  LOG_DEBUG("enabled queue qid=%d in core=%d", req->qid, req->core);
}

static void handle_disableq(struct fast_context *ctx, struct queue_entry *qe)
{
  struct guest_fast *g;
  struct proto_fast *p;
  struct queue_disableq_req *req;
  struct cham_dqueue *q;

  req = &qe->data.disableq_req;
  g = &ctx->guests[req->gid];
  p = &g->proto;
  
  q = &p->dqueues[req->qid];

  if (p->dqueues_head == q->id)
    p->dqueues_head = q->next;

  if (p->dqueues_tail == q->id)
    p->dqueues_tail = q->prev;

  if (q->next != PROTOQ_ID_INVALID)
    p->dqueues[q->next].prev = PROTOQ_ID_INVALID;

  if (q->prev != PROTOQ_ID_INVALID)
    p->dqueues[q->prev].next = PROTOQ_ID_INVALID;

  q->next = PROTOQ_ID_INVALID;
  q->prev = PROTOQ_ID_INVALID;
  p->ndqueues--;
}
#include "fast.h"
#include "queue.h"
#include "udp.h"

static int handle_new_guest(struct fast_context *ctx, struct queue_entry *qe);
static int handle_new_app(struct fast_context *ctx, struct queue_entry *qe);
static int handle_new_app_ctx(struct fast_context *ctx, struct queue_entry *qe);
static int handle_new_buf(struct fast_context *ctx, struct queue_entry *qe);
static void * select_rx(enum protocol_type type);
static void * select_tx(enum protocol_type type);
static void * select_queues(enum protocol_type type);

int slowif_poll(struct fast_context *ctx)
{
  uint8_t type;
  struct dqueue *q;
  struct queue_entry *qe;

  /* TODO: Poll up to batch size */
  q = ctx->slow_fast_q;
  qe = queue_head(q);

  if (qe == NULL)
    return 0;
  
  type = qe->type;
  switch (type)
  {
    case QUEUE_EMPTY:
      break;
    case QUEUE_ARP_TX:
      queue_dequeue(q);
      break;
    case QUEUE_NEW_GUEST:
      handle_new_guest(ctx, qe);
      queue_dequeue(q);
      break;
    case QUEUE_NEW_APP:
      handle_new_app(ctx, qe);
      queue_dequeue(q);
      break;
    case QUEUE_NEW_APP_CTX_FAST:
      handle_new_app_ctx(ctx, qe);
      queue_dequeue(q);
      break;
    case QUEUE_NEW_BUF:
      handle_new_buf(ctx, qe);
      queue_dequeue(q);
    default:
      LOG_WARN("unknown queue tryt type from slow path" 
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
  g->n_apps = 0;
  
  g->shm_base = req->shm_base;
  g->shm_len = req->shm_len;
  
  ctx->n_guests++;

  return 0;
}

static int handle_new_app(struct fast_context *ctx, struct queue_entry *qe)
{
  struct app_fast *a;
  struct guest_fast *g;
  struct queue_new_app_req *req = (struct queue_new_app_req *) &qe->data;

  a = &ctx->guests[req->gid].apps[req->id];
  a->id = req->id;
  a->proto.type = req->proto_type;
  a->proto.process_rx = select_rx(req->proto_type);
  a->proto.process_tx = select_tx(req->proto_type);
  a->proto.process_queues = select_queues(req->proto_type);
  a->n_bins = req->n_bins;
  a->buf_ht = (struct cham_buf *) req->ht_addr;
  
  g = &ctx->guests[req->gid];
  a->guest = g;

  g->n_apps++;

  return 0;
}

static int handle_new_app_ctx(struct fast_context *ctx, struct queue_entry *qe)
{
  struct app_fast *a;
  struct app_context_fast *actx;
  struct queue_new_app_ctx_fast_req *req = 
    (struct queue_new_app_ctx_fast_req *) &qe->data;

  a = &ctx->guests[req->gid].apps[req->aid];
  actx = &ctx->guests[req->gid].apps[req->aid].app_ctxs[req->cid];

  actx->id = req->cid;
  actx->app = a;
  actx->app_bump_q_off = req->app_bump_q_off;
  actx->cham_bump_q_off = req->cham_bump_q_off;

  a->n_app_ctxs++;
  
  return 0;
}

static int handle_new_buf(struct fast_context *ctx, struct queue_entry *qe)
{
  return 0;
}

static void * select_rx(enum protocol_type type)
{
  void *process_rx;

  switch(type)
  {
    case PROTO_UDP:
      process_rx = udp_process_tx;
    case PROTO_TCP:
      process_rx = NULL;
    case PROTO_RDMA:
      process_rx = NULL;
    default:
      process_rx = NULL;
  }

  return process_rx;
}

static void * select_tx(enum protocol_type type)
{
  void *process_tx;

  switch(type)
  {
    case PROTO_UDP:
      process_tx = udp_process_tx;
    case PROTO_TCP:
      process_tx = NULL;
    case PROTO_RDMA:
      process_tx = NULL;
    default:
      process_tx = NULL;
  }

  return process_tx;
}

static void * select_queues(enum protocol_type type)
{
  void *process_queue;

  switch(type)
  {
    case PROTO_UDP:
      process_queue = udp_process_queues;
    case PROTO_TCP:
      process_queue =  NULL;
    case PROTO_RDMA:
      process_queue = NULL;
    default:
      process_queue = NULL;
  }

  return process_queue;
}
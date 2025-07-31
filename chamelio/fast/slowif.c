#include "fast.h"
#include "queue.h"
#include "udp.h"

int init_new_guest(struct fast_context *ctx, struct queue_entry *qe);
int init_new_app(struct fast_context *ctx, struct queue_entry *qe);
void * select_rx(enum protocol_type type);
void * select_tx(enum protocol_type type);
void * select_queues(enum protocol_type type);

int slowif_poll(struct fast_context *ctx)
{
  uint8_t type;
  struct dqueue *q;
  struct queue_entry *qe;

  /* TODO: Poll up to batch size */
  q = ctx->slow_fast_q;
  qe = queue_head(q);
  
  if (qe != NULL)
  {
    type = qe->type;
    switch (type)
    {
      case QUEUE_EMPTY:
        break;
      case QUEUE_ARP_TX:
        queue_dequeue(q);
        break;
      case QUEUE_NEW_GUEST:
        init_new_guest(ctx, qe);
        queue_dequeue(q);
        break;
      case QUEUE_NEW_APP:
        init_new_app(ctx, qe);
        queue_dequeue(q);
        break;
      default:
        LOG_WARN("unknown queue tryt type from slow path to fast path");
        break;
    }
  }

  return 0;
}

int init_new_guest(struct fast_context *ctx, struct queue_entry *qe)
{
  struct guest_fast *g;
  struct queue_new_guest_req *req = (struct queue_new_guest_req *) &qe->data;

  g = &ctx->guests[req->id];
  g->id = req->id;
  g->n_apps = 0;
  
  g->shm_base = req->shm_base;
  g->shm_len = req->shm_len;
  
  g->next_guest = ctx->guests;
  ctx->guests = g;
  ctx->n_guests++;

  return 0;
}

int init_new_app(struct fast_context *ctx, struct queue_entry *qe)
{
  struct app_fast *a;
  struct guest_fast *g;
  struct queue_new_app_req *req = (struct queue_new_app_req *) &qe->data;

  a = &ctx->guests[req->gid].apps[req->id];
  a->id = req->id;
  a->proto.id = req->pid;
  a->proto.process_rx = select_rx(req->pid);
  a->proto.process_tx = select_tx(req->pid);
  a->proto.process_queues = select_queues(req->pid);
  
  g = &ctx->guests[req->gid];
  a->guest = g;

  a->next_app = g->apps;
  g->apps = a;
  g->n_apps++;

  return 0;
}

void * select_rx(enum protocol_type type)
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

void * select_tx(enum protocol_type type)
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

void * select_queues(enum protocol_type type)
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
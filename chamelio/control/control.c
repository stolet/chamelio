#include <stdlib.h>
#include <stdint.h>
#include <assert.h>

#include "control.h"
#include "shmalloc.h"
#include "ivshmemif.h"
#include "guestif.h"
#include "config.h"
#include "log.h"
#include "queue.h"
#include "bufs.h"

static int poll_fast(struct control_context *ctx);
static int poll_guests(struct control_context *ctx);
static int handle_new_queues_req(struct control_context *ctx,
    struct guest_control *g, struct queue_entry *qe_req);

int control_context_init(struct control_context *ctx, struct configuration *config,
    struct shm_handle **fc_handles, struct shm_handle **cf_handles)
{
  int i;
  struct guest_control *guests;
  struct equeue *cfq;
  struct dqueue *fcq;
  struct dqueue **fast_ctl_qs;
  struct equeue **ctl_fast_qs;

  ctx->config = config;
  
  ctx->ivshmem_uxfd = -1;
  ctx->ivshmem_epfd = -1;
  
  ctx->guest_uxfd = -1;
  ctx->guest_epfd = -1;

  /* Allocate pointer list for queues */
  fast_ctl_qs = malloc(sizeof(struct dqueue *) * config->fp_cores_max);
  if (fast_ctl_qs == NULL)
  {
    LOG_ERROR("failed to allocate list of fast->control queues");
    return -1;
  }
  ctx->fast_ctl_qs = fast_ctl_qs;

  ctl_fast_qs = malloc(sizeof(struct equeue *) * config->fp_cores_max);
  if (ctl_fast_qs == NULL)
  {
    LOG_ERROR("failed to alloacate list of control->fast queues");
    goto free_fast_control_list;
  }
  ctx->ctl_fast_qs = ctl_fast_qs;

  /* Create a queue with each shared memory handle */
  for (i = 0; i < config->fp_cores_max; i++)
  {
    cfq = equeue_new(config->cham_queue_len, 
        cf_handles[i]->addr, cf_handles[i]->off);
    if (cfq == NULL)
    {
      LOG_ERROR("failed to create fast to control path queue");
      goto free_control_fast_list;
    }
    ctx->ctl_fast_qs[i] = cfq;

    fcq = dqueue_new(config->cham_queue_len, 
        fc_handles[i]->addr, fc_handles[i]->off);
    if (fcq == NULL)
    {
      LOG_ERROR("failed to create control to fast path queue");
      goto free_control_fast_list;
    }
    ctx->fast_ctl_qs[i] = fcq; 
  }

  /* Allocate guests */
  guests = calloc(config->max_guests, sizeof(struct guest_control));
  if (guests == NULL)
  {
    LOG_ERROR("failed to allocate guest list");
    goto free_control_fast_list;
  }
  ctx->guests = guests;
  ctx->n_guests = 0;

  return 0;

free_control_fast_list:
  free(ctl_fast_qs);
free_fast_control_list:
  free(fast_ctl_qs);
  return -1;
}

int control_loop(struct control_context *ctx)
{
  int ret;

  ret = ivshmemif_init(ctx);
  if (ret != 0)
  {
    LOG_ERROR("failed to initialise ivshmemif");
    return -1;
  }

  ret = guestif_init(ctx);
  if (ret != 0)
  {
    LOG_ERROR("failed to initialise appif");
    return -1;
  }

  while (1) 
  {
    ivshmemif_poll(ctx);
    guestif_poll(ctx);
    poll_fast(ctx);
    poll_guests(ctx);
  }
}

/* Polls for messages from fast-path */
static int poll_fast(struct control_context *ctx)
{
  struct dqueue *q;
  struct queue_entry *qe;
  int i;

  for (i = 0; i < ctx->config->fp_cores_max; i++)
  {
    /* TODO: Dequeue in batches to improve performance and prevent us
       from spending too much time in one core */
    q = ctx->fast_ctl_qs[i];
    qe = queue_head(q);

    /* Queue is empty */
    if (qe == NULL)
      continue;

    switch (qe->type)
    {
      case QUEUE_EMPTY:
        break;
      default:
        LOG_ERROR("unknown queue entry type from "
            "fast path to control path type=%d", qe->type);
        abort();
    }
  }

  return 0;
}

/* Polls for messages from guests */
static int poll_guests(struct control_context *ctx)
{
  struct dqueue *q;
  struct queue_entry *qe;
  struct guest_control *g;
  int i;

  for (i = 0; i < ctx->n_guests; i++)
  {
    /* TODO: Dequeue in batches to improve performance and prevent us
       from spending too much time in one core */
    g = &ctx->guests[i];
    q = g->guest_cham_q;
    qe = queue_head(q);

    /* Queue is empty */
    if (qe == NULL)
      continue;

    switch (qe->type)
    {
      case QUEUE_NEW_QUEUES_REQ:
        handle_new_queues_req(ctx, g, qe);
        queue_dequeue(q);
        break;
      default:
        LOG_ERROR("unknown queue entry type from "
            "guest to control path type=%d", qe->type);
        abort();
    }
  }

  return 0;  
}

static int handle_new_queues_req(struct control_context *ctx,
    struct guest_control *g, struct queue_entry *qe_req)
{
  int i, j, ret;
  struct queue_entry *qe_res;
  struct queue_new_queues_req *g_req, *c_req;
  struct queue_new_queues_res *res;
  struct shm_handle *sh;

  g_req = (struct queue_new_queues_req *) &qe_req->data;
  
  qe_res = queue_tail(g->cham_guest_q);
  assert(qe_res != NULL);
  res = (struct queue_new_queues_res *) &qe_res->data;
 
  if (g_req->nqueues > MAX_PROTO_QUEUES)
  {
    LOG_WARN("requested more queues than the maximum supported");
    res->nqueues = 0;
    ret = queue_enqueue(g->cham_guest_q, QUEUE_NEW_QUEUES_RES);
    assert(ret == 0);
    return -1;
  }

  /* Allocate each requested queue */
  for (i = 0; i < g_req->nqueues; i++)
  {
    ret = shmalloc_alloc(g->alloc, g_req->elsize * g_req->nelems, &sh);
    if (ret != 0)
    {
      LOG_ERROR("failed to allocate memory for queue=%d", i);
      res->nqueues = 0;
      ret = queue_enqueue(g->cham_guest_q, QUEUE_NEW_QUEUES_RES);
      assert(ret == 0);
      return -1;
    }

    g->proto.queue_offs[i] = sh->off;
    res->offs[i] = sh->off;
  }

  /* Send request for new queues to the fast-path */
  for (i = 0; i < ctx->config->fp_cores_max; i++)
  {
    qe_req = queue_tail(ctx->ctl_fast_qs[i]);
    assert(qe_req != NULL);
    c_req = (struct queue_new_queues_req *) &qe_req->data;
    c_req->gid = g->id;
    c_req->elsize = g_req->elsize;
    c_req->nelems = g_req->nelems;
    c_req->nqueues = g_req->nqueues;

    for (j = 0; j < g_req->nqueues; j++)
      c_req->offs[j] = res->offs[i];

    ret = queue_enqueue(ctx->ctl_fast_qs[i], QUEUE_NEW_QUEUES_REQ);
    assert(ret == 0);
  }

  /* Send response back to guest */
  res->nqueues = g_req->nqueues;
  ret = queue_enqueue(g->cham_guest_q, QUEUE_NEW_QUEUES_RES);
  assert(ret == 0);
  return 0;
}
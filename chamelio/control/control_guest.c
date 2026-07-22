#include <assert.h>
#include <linux/types.h>
#include <string.h>

#include "control_guest.h"
#include "log.h"
#include "queue_fns.h"

void control_guest_new_proto(struct control_context *ctx,
    struct guest_control *g, struct queue_entry *qe_req)
{
  int ret;
  struct queue_entry *qe_res;
  struct queue_new_proto_req *req;
  struct queue_new_proto_res *res;
  struct proto_control *p;

  p = &g->proto;
  req = &qe_req->data.new_proto_req;

  /* Initialize response */
  qe_res = queue_tail(g->cham_guest_q);
  assert(qe_res != NULL);
  res = &qe_res->data.new_proto_res;
  res->success = 0;
  res->n_fp_cores = 0;
  res->shm_len = 0;
  res->local_ip = 0;

  switch (req->proto_type)
  {
    case CHAM_PROTO_TCP:
    case CHAM_PROTO_UDP:
    case CHAM_PROTO_RPC:
      break;
    default:
      ret = queue_enqueue(g->cham_guest_q, QUEUE_NEW_PROTO_RES);
      assert(ret == 0);
      return;
  }

  switch (ctx->config->fp_proto_mode)
  {
    case FP_PROTO_EBPF:
    case FP_PROTO_HAND:
      break;
    default:
      ret = queue_enqueue(g->cham_guest_q, QUEUE_NEW_PROTO_RES);
      assert(ret == 0);
      return;
  }

  p->guest = g;
  p->proto_type = req->proto_type;
  p->nqueues = 0;
  p->nmaps = 0;
  res->success = 1;
  res->n_fp_cores = ctx->config->fp_cores_max;
  res->shm_len = ctx->config->shm_len;
  res->local_ip = ctx->config->ip;

  ret = queue_enqueue(g->cham_guest_q, QUEUE_NEW_PROTO_RES);
  assert(ret == 0);
}

void control_guest_new_queue(struct control_context *ctx,
    struct guest_control *g, struct queue_entry *qe_req)
{
  int i, ret;
  __u16 nqueues;
  struct queue_entry *qe_res;
  struct queue_new_queue_req *req, *req_fast;
  struct queue_new_queue_res *res;
  struct shm_handle *sh;

  nqueues = g->proto.nqueues;
  g->proto.nqueues++;
  req = &qe_req->data.new_queue_req;

  qe_res = queue_tail(g->cham_guest_q);
  assert(qe_res != NULL);
  res = &qe_res->data.new_queue_res;

  if (nqueues >= MAX_PROTO_QUEUES)
  {
    LOG_WARN("requested more queues than the maximum supported");
    res->elsize = 0;
    res->nelems = 0;
    ret = queue_enqueue(g->cham_guest_q, QUEUE_NEW_QUEUE_RES);
    assert(ret == 0);
    return;
  }

  /* Allocate requested queue */
  ret = shmalloc_alloc(g->alloc, req->elsize * req->nelems, &sh);
  if (ret != 0)
  {
    LOG_ERROR("failed to allocate memory for queue=%d", nqueues);
    res->elsize = 0;
    res->nelems = 0;
    ret = queue_enqueue(g->cham_guest_q, QUEUE_NEW_QUEUE_RES);
    assert(ret == 0);
    return;
  }
  memset(sh->addr, 0, sh->len);

  g->proto.queues[nqueues].id = nqueues;
  g->proto.queues[nqueues].nelems = req->nelems;
  g->proto.queues[nqueues].elsize = req->elsize;
  g->proto.queues[nqueues].off = sh->off;
  g->proto.queues[nqueues].core = CORE_INVALID;
  res->off = sh->off;

  /* Register queue with each fast-path core */
  for (i = 0; i < ctx->config->fp_cores_max; i++)
  {
    qe_req = queue_tail(ctx->ctl_fast_qs[i]);
    assert(qe_req != NULL);
    req_fast = &qe_req->data.new_queue_req;
    req_fast->gid = g->id;
    req_fast->proto_type = g->proto.proto_type;
    req_fast->qid = g->proto.queues[nqueues].id;
    req_fast->nelems = g->proto.queues[nqueues].nelems;
    req_fast->elsize = g->proto.queues[nqueues].elsize;
    req_fast->off = g->proto.queues[nqueues].off;

    ret = queue_enqueue(ctx->ctl_fast_qs[i], QUEUE_NEW_QUEUE_REQ);
    assert(ret == 0);
  }

  /* Send response back to guest */
  res->qid = nqueues;
  res->nelems = req->nelems;
  res->elsize = req->elsize;
  res->off = sh->off;
  res->opaque = req->opaque;
  ret = queue_enqueue(g->cham_guest_q, QUEUE_NEW_QUEUE_RES);
  assert(ret == 0);
}

void control_guest_new_map(struct control_context *ctx,
    struct guest_control *g, struct queue_entry *qe_req)
{
  int i, ret;
  struct queue_entry *qe_res;
  struct queue_new_map_req *g_req, *c_req;
  struct queue_new_map_res *res;
  struct shm_handle *sh;

  g_req = &qe_req->data.new_map_req;

  qe_res = queue_tail(g->cham_guest_q);
  assert(qe_res != NULL);
  res = (struct queue_new_map_res *)&qe_res->data;

  if (g->proto.nmaps >= MAX_PROTO_MAPS)
  {
    LOG_WARN("requested more maps than the maximum supported");
    res->nelems = 0;
    res->elsize = 0;
    ret = queue_enqueue(g->cham_guest_q, QUEUE_NEW_MAP_RES);
    assert(ret == 0);
    return;
  }

  /* Allocate requested map */
  ret = shmalloc_alloc(g->alloc, g_req->elsize * g_req->nelems, &sh);
  if (ret != 0)
  {
    LOG_ERROR("failed to allocate memory for map");
    res->nelems = 0;
    res->elsize = 0;
    ret = queue_enqueue(g->cham_guest_q, QUEUE_NEW_MAP_RES);
    assert(ret == 0);
    return;
  }

  res->id = g->proto.nmaps;
  res->off = sh->off;
  res->elsize = g_req->elsize;
  res->nelems = g_req->nelems;
  res->opaque = g_req->opaque;
  g->proto.nmaps++;

  /* Send request for maps to the fast-path */
  for (i = 0; i < ctx->config->fp_cores_max; i++)
  {
    qe_req = queue_tail(ctx->ctl_fast_qs[i]);
    assert(qe_req != NULL);
    c_req = &qe_req->data.new_map_req;
    c_req->gid = g->id;
    c_req->proto_type = g->proto.proto_type;
    c_req->mid = res->id;
    c_req->elsize = g_req->elsize;
    c_req->nelems = g_req->nelems;
    c_req->off = sh->off;

    ret = queue_enqueue(ctx->ctl_fast_qs[i], QUEUE_NEW_MAP_REQ);
    assert(ret == 0);
  }

  /* Send response back to guest */
  ret = queue_enqueue(g->cham_guest_q, QUEUE_NEW_MAP_RES);
  assert(ret == 0);
}

void control_guest_enableq(struct control_context *ctx,
    struct guest_control *g, struct queue_entry *qe)
{
  int ret;
  struct equeue *q;
  struct proto_queue_control *pq;
  struct queue_enableq_req *req, *req_fast;

  req = &qe->data.enableq_req;

  if (req->core >= ctx->config->fp_cores_max)
  {
    LOG_WARN("tried to enable queue in nonexistent core");
    return;
  }

  if (req->qid >= g->proto.nqueues)
  {
    LOG_WARN("tried to access nonexistent queue");
    return;
  }

  q = ctx->ctl_fast_qs[req->core];
  qe = queue_tail(q);
  assert(qe != NULL);

  pq = &g->proto.queues[req->qid];
  req_fast = (struct queue_enableq_req *)&qe->data;
  req_fast->gid = g->id;
  req_fast->proto_type = g->proto.proto_type;
  req_fast->qid = req->qid;
  req_fast->off = pq->off;
  req_fast->nelems = pq->nelems;
  req_fast->elsize = pq->elsize;
  req_fast->core = req->core;
  g->proto.queues[req->qid].core = req->core;

  /* Enable queue in fast-path */
  ret = queue_enqueue(q, QUEUE_ENABLEQ_REQ);
  assert(ret == 0);
}

void control_guest_disableq(struct control_context *ctx,
    struct guest_control *g, struct queue_entry *qe)
{
  int ret;
  struct equeue *q;
  struct queue_disableq_req *req, *req_fast;

  req = &qe->data.disableq_req;

  if (req->core >= ctx->config->fp_cores_max)
  {
    LOG_WARN("tried to disable queue in nonexistent core");
    return;
  }

  if (req->qid >= g->proto.nqueues)
  {
    LOG_WARN("tried to access nonexistent queue");
    return;
  }

  q = ctx->ctl_fast_qs[req->core];
  qe = queue_tail(q);
  assert(qe != NULL);

  req_fast = (struct queue_disableq_req *)&qe->data;
  req_fast->gid = g->id;
  req_fast->qid = req->qid;
  req_fast->core = req->core;

  g->proto.queues[req->qid].core = req->core;

  /* Enable queue in fast-path */
  ret = queue_enqueue(q, QUEUE_DISABLEQ_REQ);
  assert(ret == 0);
}

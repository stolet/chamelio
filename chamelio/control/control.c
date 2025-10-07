#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>

#include "control.h"
#include "shmalloc.h"
#include "ivshmemif.h"
#include "guestif.h"
#include "config.h"
#include "log.h"
#include "queue.h"

#include "ebpfif.h"

static int poll_fast(struct control_context *ctx);
static int poll_guests(struct control_context *ctx);
static int handle_new_queue_req(struct control_context *ctx,
                                struct guest_control *g, struct queue_entry *qe_req);
static int handle_new_map_req(struct control_context *ctx,
                              struct guest_control *g, struct queue_entry *qe_req);
static int handle_enableq_req(struct control_context *ctx,
                              struct guest_control *g, struct queue_entry *qe);
static int handle_disableq_req(struct control_context *ctx,
                               struct guest_control *g, struct queue_entry *qe);
static int handle_allocate_ebpf_req(struct guest_control *g, struct queue_entry *qe_req);
static int handle_free_ebpf_req(struct control_context *ctx,
                                struct guest_control *g, struct queue_entry *qe_req);
static int handle_upload_ebpf_req(struct control_context *ctx,
                                  struct guest_control *g, struct queue_entry *qe_req);
static int jit_ebpf(void *ebpf_bytecode, size_t size);

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
    cfq = equeue_new(config->cham_queue_len, sizeof(struct queue_entry),
                     cf_handles[i]->addr, cf_handles[i]->off);
    if (cfq == NULL)
    {
      LOG_ERROR("failed to create fast to control path queue");
      goto free_control_fast_list;
    }
    ctx->ctl_fast_qs[i] = cfq;

    fcq = dqueue_new(config->cham_queue_len, sizeof(struct queue_entry),
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
                "fast path to control path type=%d",
                qe->type);
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
    case QUEUE_NEW_QUEUE_REQ:
      handle_new_queue_req(ctx, g, qe);
      queue_dequeue(q);
      break;
    case QUEUE_NEW_MAP_REQ:
      handle_new_map_req(ctx, g, qe);
      queue_dequeue(q);
      break;
    case QUEUE_ENABLEQ_REQ:
      handle_enableq_req(ctx, g, qe);
      queue_dequeue(q);
      break;
    case QUEUE_DISABLEQ_REQ:
      handle_disableq_req(ctx, g, qe);
      queue_dequeue(q);
      break;
    case QUEUE_ALLOCATE_EBPF_REQ:
      handle_allocate_ebpf_req(g, qe);
      queue_dequeue(q);
      break;
    case QUEUE_FREE_EBPF_REQ:
      handle_free_ebpf_req(ctx, g, qe);
      queue_dequeue(q);
      break;
    case QUEUE_UPLOAD_EBPF_REQ:
      handle_upload_ebpf_req(ctx, g, qe);
      queue_dequeue(q);
      break;
    default:
      LOG_ERROR("unknown queue entry type from "
                "guest to control path type=%d",
                qe->type);
      abort();
    }
  }

  return 0;
}

static int handle_new_queue_req(struct control_context *ctx,
                                struct guest_control *g, struct queue_entry *qe_req)
{
  int i, ret;
  uint16_t nqueues;
  struct queue_entry *qe_res;
  struct queue_new_queue_req *req, *req_fast;
  struct queue_new_queue_res *res;
  struct shm_handle *sh;

  nqueues = g->proto.nqueues;
  g->proto.nqueues++;
  req = &qe_req->data.new_queue_req;

  qe_res = queue_tail(g->cham_guest_q);
  assert(qe_res != NULL);
  res = (struct queue_new_queue_res *)&qe_res->data;

  if (nqueues >= MAX_PROTO_QUEUES)
  {
    LOG_WARN("requested more queues than the maximum supported");
    res->elsize = 0;
    res->nelems = 0;
    ret = queue_enqueue(g->cham_guest_q, QUEUE_NEW_QUEUE_RES);
    assert(ret == 0);
    return -1;
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
    return -1;
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
  return 0;
}

static int handle_new_map_req(struct control_context *ctx,
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
    return -1;
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
    return -1;
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
  return 0;
}

static int handle_enableq_req(struct control_context *ctx,
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
    return -1;
  }

  if (req->qid >= g->proto.nqueues)
  {
    LOG_WARN("tried to access nonexistent queue");
    return -1;
  }

  q = ctx->ctl_fast_qs[req->core];
  qe = queue_tail(q);
  assert(qe != NULL);

  pq = &g->proto.queues[req->qid];
  req_fast = (struct queue_enableq_req *)&qe->data;
  req_fast->gid = g->id;
  req_fast->qid = req->qid;
  req_fast->off = pq->off;
  req_fast->nelems = pq->nelems;
  req_fast->elsize = pq->elsize;
  req_fast->core = req->core;
  g->proto.queues[req->qid].core = req->core;

  /* Enable queue in fast-path */
  ret = queue_enqueue(q, QUEUE_ENABLEQ_REQ);
  assert(ret == 0);

  return 0;
}

static int handle_disableq_req(struct control_context *ctx,
                               struct guest_control *g, struct queue_entry *qe)
{
  int ret;
  struct equeue *q;
  struct queue_disableq_req *req, *req_fast;

  req = &qe->data.disableq_req;

  if (req->core >= ctx->config->fp_cores_max)
  {
    LOG_WARN("tried to disable queue in nonexistent core");
    return -1;
  }

  if (req->qid >= g->proto.nqueues)
  {
    LOG_WARN("tried to access nonexistent queue");
    return -1;
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

  return 0;
}

// TODO: do I actually need these ctx?

static int handle_allocate_ebpf_req(struct guest_control *g, struct queue_entry *qe_req)
{
  int ret;
  struct queue_allocate_ebpf_req *req;
  struct shm_handle *sh;
  req = &qe_req->data.alloc_ebpf_req;
  struct queue_entry *qe_res = queue_tail(g->cham_guest_q);
  assert(qe_res != NULL);
  struct queue_allocate_ebpf_res *res = (struct queue_allocate_ebpf_res *)&qe_res->data;

  ret = shmalloc_alloc(g->alloc, req->size, &sh);
  if (ret != 0)
  {
    LOG_ERROR("failed to allocate memory for eBPF program");
    res->size = 0;
    res->off = 0;
    res->opaque = req->opaque;
    ret = queue_enqueue(g->cham_guest_q, QUEUE_ALLOCATE_EBPF_RES);
    assert(ret == 0); // to ensure queue-enqueue succeeds
    return -1;
  }
  memset(sh->addr, 0, sh->len);
  res->size = req->size;
  res->off = sh->off;
  res->opaque = req->opaque;
  ret = queue_enqueue(g->cham_guest_q, QUEUE_ALLOCATE_EBPF_RES);
  assert(ret == 0);
  return 0;
}

static int handle_upload_ebpf_req(struct control_context *ctx,
                                  struct guest_control *g, struct queue_entry *qe_req)
{
  int ret, i;
  struct queue_free_up_ebpf_req *req, *f_req;
  struct queue_entry *qe_res;
  struct queue_free_up_ebpf_res *res;
  void *ebpf_bytecode;

  req = &qe_req->data.free_up_ebpf_req;

  qe_res = queue_tail(g->cham_guest_q);
  assert(qe_res != NULL);
  res = (struct queue_free_up_ebpf_res *)&qe_res->data;
  
  ebpf_bytecode = (uint8_t *)g->shm_base + req->off;

  ret = jit_ebpf(ebpf_bytecode, req->size);

  if (ret != 0)
  {
    LOG_ERROR("failed to JIT eBPF program");
    res->success = -1; // indicating upload failed 
    ret = queue_enqueue(g->cham_guest_q, QUEUE_UPLOAD_EBPF_RES);
    assert(ret == 0);
    return -1;
  }
  
  /*
  Verify the EBPF bytecode
  JIT it (done)
  Load it to fast path through function pointers
  Return success or failure
  */
  for (i = 0; i < ctx->config->fp_cores_max; i++)
  {
    qe_req = queue_tail(ctx->ctl_fast_qs[i]);
    assert(qe_req != NULL);
    f_req = &qe_req->data.free_up_ebpf_req;
    f_req->size = req->size;
    f_req->off = req->off;

    ret = queue_enqueue(ctx->ctl_fast_qs[i], QUEUE_UPLOAD_EBPF_REQ);
    assert(ret == 0);
  }

  res = (struct queue_free_up_ebpf_res *)&qe_res->data;
  //send response back to guest
  res->success = 0; // indicating upload was successful
  ret = queue_enqueue(g->cham_guest_q, QUEUE_UPLOAD_EBPF_RES);
  assert(ret == 0);
  return 0; // indicating success for now
}

// TODO: do I actually need these ctx?
static int handle_free_ebpf_req(struct control_context *ctx,
                                struct guest_control *g, struct queue_entry *qe_req)
{
  int ret;
  struct queue_free_up_ebpf_req *req;
  struct shm_handle *sh;
  req = &qe_req->data.free_up_ebpf_req;
  struct queue_free_up_ebpf_res *res;

  struct queue_entry *qe_res; 
  qe_res = queue_tail(g->cham_guest_q);
  assert(qe_res != NULL);
  
  res = (struct queue_free_up_ebpf_res *)&qe_res->data;

  // TODO: modify this to use handler
  // shmalloc_free(g->alloc, &sh);

  // TODO: implement shmalloc_free with offset and size only instead of handle
  //  not convenient as we need to find a matching offset and size everytime.
  // TODO: modify the ebpf struct to include the handle instead of off and size

  // return success for now without freeing
  //TODO: unload the vm code and destroy it 

  res->success = 0; // indicating free was successful
  ret = queue_enqueue(g->cham_guest_q, QUEUE_FREE_EBPF_RES);
  assert(ret == 0);
  return 0;
}

static int verify_ebpf(void *ebpf_bytecode, size_t size)
{ 
  return 0;
}

static uint64_t nop_helper(uint64_t r1, uint64_t r2, uint64_t r3,
                           uint64_t r4, uint64_t r5) {
  (void)r1; (void)r2; (void)r3; (void)r4; (void)r5;
  return 0;
}

//pointer to the memory with the jitted code inside the llvmbpf_vm_c struct: llvmbpf_jitted_fn
static int jit_ebpf(void *ebpf_bytecode, size_t size)
{
  uint64_t res;
  struct llvmbpf_vm_c *vm;
  vm = llvmbpf_vm_create();
  if (vm == NULL)
  {
    LOG_ERROR("failed to create llvmbpf vm");
    return -1;
  }
  res = llvmbpf_vm_load_code(vm, ebpf_bytecode, size);
  if (res != 0)
  {
    LOG_ERROR("failed to load ebpf bytecode");
    return res;
  }
  // Register helper functions here
  res = llvmbpf_vm_register_helper(vm, 2, nop_helper, "nop_helper");
  if (res != 0)
  {
    LOG_ERROR("failed to register helper function");
    return res;
  }

  res = llvmbpf_vm_compile(vm); // LLVM JIT
  if (res != 0)
  {
    LOG_ERROR("failed to JIT ebpf bytecode");
    return res;
  }
  return res;
}

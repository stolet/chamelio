#include <stdlib.h>
#include <linux/types.h>
#include <assert.h>
#include <string.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include <rte_ip4.h>

#include "control.h"
#include "tomgr.h"
#include "clock.h"
#include "shmalloc.h"
#include "nic.h"
#include "ivshmemif.h"
#include "guestif.h"
#include "config.h"
#include "log.h"
#include "queue_fns.h"
#include "queue_types.h"
#include "scheduler_fns.h"
#include "ebpf.h"
#include "verifier.h"
#include "cham_fast.h"

static inline int poll_fast(struct control_context *ctx);
static inline int poll_guests(struct control_context *ctx);
static inline int poll_timeouts(struct control_context *ctx);

static inline void handle_arp_lookup(struct control_context *ctx, 
    struct queue_entry *qe);
static inline void handle_arp_req(struct control_context *ctx,
  struct queue_entry *qe);
static inline void handle_arp_rep(struct control_context *ctx,
  struct queue_entry *qe);
static inline void handle_arp_timeout(struct control_context *ctx,
    struct to_entry *ae);

static inline void handle_new_queue_req(struct control_context *ctx,
    struct guest_control *g, struct queue_entry *qe_req);
static inline void handle_new_map_req(struct control_context *ctx,
    struct guest_control *g, struct queue_entry *qe_req);
static inline void handle_enableq_req(struct control_context *ctx,
    struct guest_control *g, struct queue_entry *qe);
static inline void handle_disableq_req(struct control_context *ctx,
    struct guest_control *g, struct queue_entry *qe);
    
static inline void handle_allocate_ebpf_req(struct guest_control *g, 
    struct queue_entry *qe_req);
static inline void handle_free_ebpf_req(struct guest_control *g, 
    struct queue_entry *qe_req);
static inline void handle_upload_ebpf_req(struct control_context *ctx,
    struct guest_control *g, struct queue_entry *qe_req);
static inline struct ebpf_vm_c * jit_ebpf(const void *ebpf_instrs, 
    size_t size);

static void ebpf_print(int a);
static inline void * ebpf_memcpy(void *dst, void *src, size_t n);
static inline __u16 ebpf_ipv4_checksum(void *ip_hdr);
static inline __u16 ebpf_ipv4_udptcp_cksum(void *ip_hdr, void *udp_hdr);
static inline void * ebpf_map_get(void *map_base, __u32 len);
static inline void * ebpf_map_lookup(void *map_base, __u64 id, __u64 elsize);
static inline void * ebpf_queue_tail(struct equeue *q, __u64 elsize);

int control_context_init(struct control_context *ctrl_ctx, 
    struct nic_context *nic_ctx, struct configuration *config, 
    struct shm_handle **fc_handles, struct shm_handle **cf_handles,
    struct shm_handle **txq_handles)
{
  int i;
  struct tomgr *tomgr;
  struct guest_control *guests;
  struct equeue *cfq, *txq;
  struct dqueue *fcq;
  struct dqueue **fast_ctl_qs;
  struct equeue **ctl_fast_qs, **txqs;

  ctrl_ctx->config = config;
  ctrl_ctx->nic_ctx = nic_ctx;

  ctrl_ctx->ivshmem_uxfd = -1;
  ctrl_ctx->ivshmem_epfd = -1;

  ctrl_ctx->guest_uxfd = -1;
  ctrl_ctx->guest_epfd = -1;

  /* Initialize ARP table to default values */
  arp_table_init(&ctrl_ctx->arp_table);
  
  /* Initialize timeout manager */
  tomgr = tomgr_init();
  if (tomgr == NULL)
  {
    LOG_ERROR("failed to initialise timeout manager");
    return -1;
  }
  ctrl_ctx->tomgr = tomgr;

  /* Calibrate tsc so we can get accurate time */
  if (clock_calibrate_tsc() != 0)
  {
    LOG_ERROR("failed to calibrate tsc");
    return -1;
  }
  
  /* Allocate pointer list for queues from fast to control */
  fast_ctl_qs = malloc(sizeof(struct dqueue *) * config->fp_cores_max);
  if (fast_ctl_qs == NULL)
  {
    LOG_ERROR("failed to allocate list of fast->control queues");
    goto free_tomgr;
  }
  ctrl_ctx->fast_ctl_qs = fast_ctl_qs;
  ctrl_ctx->next_core = 0;

  /* Allocate pointer list for queues from control to fast */
  ctl_fast_qs = malloc(sizeof(struct equeue *) * config->fp_cores_max);
  if (ctl_fast_qs == NULL)
  {
    LOG_ERROR("failed to alloacate list of control->fast queues");
    goto free_fast_control_list;
  }
  ctrl_ctx->ctl_fast_qs = ctl_fast_qs;

  /* Allocate pointer list for queues containing packets for transmission */
  txqs = malloc(sizeof (struct equeue *) * config->fp_cores_max);
  if (txqs == NULL)
  {
    LOG_ERROR("failed to allocate list of control-path transmit queues");
    goto free_control_fast_list;
  }
  ctrl_ctx->txqs = txqs;
  
  /* Create a queue with each shared memory handle */
  for (i = 0; i < config->fp_cores_max; i++)
  {
    cfq = equeue_new(config->cham_queue_len, sizeof(struct queue_entry),
                     cf_handles[i]->addr, cf_handles[i]->off);
    if (cfq == NULL)
    {
      LOG_ERROR("failed to create fast to control path queue");
      goto free_control_txqs;
    }
    ctrl_ctx->ctl_fast_qs[i] = cfq;

    fcq = dqueue_new(config->cham_queue_len, sizeof(struct queue_entry),
                     fc_handles[i]->addr, fc_handles[i]->off);
    if (fcq == NULL)
    {
      LOG_ERROR("failed to create control to fast path queue");
      goto free_control_txqs;
    }
    ctrl_ctx->fast_ctl_qs[i] = fcq;
    
    txq = equeue_new(config->control_txq_len, config->control_txq_pkt_len, 
        txq_handles[i]->addr, txq_handles[i]->off);
    if (txq == NULL)
    {
      LOG_ERROR("failed to create control path transmit queue");
      goto free_control_txqs;
    }
    ctrl_ctx->txqs[i] = txq;
  }

  /* Allocate guests */
  guests = calloc(config->max_guests, sizeof(struct guest_control));
  if (guests == NULL)
  {
    LOG_ERROR("failed to allocate guest list");
    goto free_control_fast_list;
  }
  ctrl_ctx->guests = guests;
  ctrl_ctx->n_guests = 0;
  ctrl_ctx->next_guest = 0;

  return 0;

free_control_txqs:
  free(txqs);
free_control_fast_list:
  free(ctl_fast_qs);
free_fast_control_list:
  free(fast_ctl_qs);
free_tomgr:
  free(tomgr);
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
    poll_timeouts(ctx);
  }
}

/* Polls for messages from fast-path */
static inline int poll_fast(struct control_context *ctx)
{
  int i, cores_polled, increment_core;
  struct dqueue *q;
  struct queue_entry *qe;

  i = 0;
  cores_polled = 0;
  increment_core = 0;
  while (i < BATCH_SIZE)
  {
    if (cores_polled >= ctx->config->fp_cores_max)
      break;

    q = ctx->fast_ctl_qs[ctx->next_core];
    qe = queue_head(q);

    /* Queue is empty */
    if (qe == NULL)
    {
      cores_polled++;
      increment_core = 0;
      ctx->next_core = (ctx->next_core + 1) % ctx->config->fp_cores_max;
      continue;
    }

    increment_core = 1;
    i++;
    switch (qe->type)
    {
      case QUEUE_EMPTY:
        break;
      case QUEUE_ARP_LOOKUP:
        handle_arp_lookup(ctx, qe);
        queue_dequeue(q);
        break;
      case QUEUE_ARP_RX_REQ:
        handle_arp_req(ctx, qe);
        queue_dequeue(q);
        break;
      case QUEUE_ARP_RX_REP:
        handle_arp_rep(ctx, qe);
        queue_dequeue(q);
        break;
      default:
        LOG_ERROR("unknown queue entry type from "
                  "fast path to control path type=%d",
                  qe->type);
        abort();
    }
  }

  /* Don't double increment core when last iteration had empty queue */
  if (increment_core)
    ctx->next_core = (ctx->next_core + 1) % ctx->config->fp_cores_max;

  return 0;
}

/* Polls for messages from guests */
static inline int poll_guests(struct control_context *ctx)
{
  struct dqueue *q;
  struct queue_entry *qe;
  struct guest_control *g;
  int i, guests_polled, increment_guest;

  i = 0;
  guests_polled = 0;
  increment_guest = 0;
  while (i < BATCH_SIZE)
  {
    if (guests_polled >= ctx->n_guests)
      break;

    g = &ctx->guests[ctx->next_guest];
    q = g->guest_cham_q;
    qe = queue_head(q);

    /* Queue is empty */
    if (qe == NULL)
    {
      guests_polled++;
      increment_guest = 0;
      ctx->next_guest = (ctx->next_guest + 1) % ctx->n_guests;
      continue;
    }

    increment_guest = 1;
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
        handle_free_ebpf_req(g, qe);
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

  /* Don't double increment guest when last iteration had empty queue */
  if (increment_guest)
    ctx->next_guest = (ctx->next_guest + 1) % ctx->n_guests;

  return 0;
}

static inline int poll_timeouts(struct control_context *ctx)
{
  int i;
  struct to_entry *te;
  
  for (i = 0; i < BATCH_SIZE; i++)
  {
    te = tomgr_peek(ctx->tomgr);
    if (te == NULL)
      break;
      
    /* This entry timed out */
    if (te->to < clock_rdtsc())
    {
      switch (te->type)
      {
      case TO_ARP:
        handle_arp_timeout(ctx, te);
        break;
      default:
        break;
      }
      
      te = tomgr_pop(ctx->tomgr);
      if (te == NULL)
      {
        LOG_ERROR("failed to pop timeout manager");
        return -1;
      }
    }
  }
  
  return 0;
}

static inline void handle_arp_timeout(struct control_context *ctx, 
    struct to_entry *te)
{
  int ret;
  struct arp_entry *ae = (struct arp_entry *) te->data;
  
  /* If this entry is not pending anymore return */
  if (!ae->pending)
    return;
    
  /* Send another ARP request */
  ret = arp_request(ctx->txqs[0], ctx->ctl_fast_qs[0], ae->ip,
        (__u8 *) &ctx->nic_ctx->eth_addr.addr_bytes, ctx->config->ip);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue ARP request");
    return;
  }
  
  /* Enqueue a new timeout */
  te = tomgr_insert(ctx->tomgr, TO_ARP, clock_tsc_after_us(ARP_TIMEOUT), ae);
  if (te == NULL)
  {
    LOG_ERROR("failed to insert ARP timeout");
    return;
  }
  ae->te = te;
  
  return;
}

static inline void handle_arp_lookup(struct control_context *ctx, 
    struct queue_entry *qe)
{
  int ret;
  struct arp_entry *ae;
  struct to_entry *te;
  struct queue_arp_lookup *le;
  
  le = &qe->data.arp_lookup;
  
  /* Ignore lookup if this address is resolved or pending */
  ae = arp_lookup(&ctx->arp_table, le->ip);
  
  if (ae == NULL)
  {
    /* Insert as pending to ARP table */
    ae = arp_insert_pending(&ctx->arp_table, le->ip);
    
    /* Enqueue ARP request to fast-path */
    ret = arp_request(ctx->txqs[0], ctx->ctl_fast_qs[0], le->ip,
        (__u8 *) &ctx->nic_ctx->eth_addr.addr_bytes, ctx->config->ip);
    if (ret != 0)
    {
      LOG_ERROR("failed to enqueue ARP request");
      return;
    }
    
    /* Insert timeout for ARP request */
    te = tomgr_insert(ctx->tomgr, TO_ARP, clock_tsc_after_us(ARP_TIMEOUT), ae);
    if (te == NULL)
    {
      LOG_ERROR("failed to insert timeout for ARP request");
      return;
    }
    ae->te = te;
  }
  
      
  return;
}

static inline void handle_arp_req(struct control_context *ctx,
  struct queue_entry *qe)
{
  int ret, i;
  struct queue_entry *arp_up;
  struct arp_entry *ae;
  struct queue_arp_rx_req *arp_req = &qe->data.arp_pkt_rx_req;
  
  /* Check if ARP request was for me*/
  if (arp_req->tpa != ctx->config->ip)
    return;
    
  ae = arp_lookup(&ctx->arp_table, arp_req->spa);
  if (ae == NULL || ae->pending)
  {
    /* Add sender to ARP table */
    if (arp_insert(&ctx->arp_table, arp_req->spa, 
        (__u8 *) &arp_req->sha) == NULL)
    {
      LOG_ERROR("failed to add sender to ARP table");
      return;
    }
    
    /* TODO: Don't duplicate this code */
    /* Tell fast-path to update ARP tables */
    for (i = 0; i < ctx->config->fp_cores_max; i++)
    {
      arp_up = queue_tail(ctx->ctl_fast_qs[i]);
      if (arp_up == NULL)
      {
        LOG_ERROR("failed to get tail of control->fast queue");
        return;
      }
      
      arp_up->data.arp_update.ip = arp_req->spa;
      memcpy(&arp_up->data.arp_update.mac, &arp_req->sha, ETH_ADDR_LEN);
      
      ret = queue_enqueue(ctx->ctl_fast_qs[i], QUEUE_ARP_UPDATE);
      if (ret != 0)
      {
        LOG_ERROR("failed to enqueue ARP update to control->fast queue");
        return;
      }
    }
  }
  
  /* Enqueue ARP reply for fast-path */
  ret = arp_reply(ctx->txqs[0], ctx->ctl_fast_qs[0],
      (__u8 *) &ctx->nic_ctx->eth_addr.addr_bytes,
      ctx->config->ip, (__u8 *) &arp_req->sha, arp_req->spa);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue ARP reply");
    return;
  }
  
  return;
}

static inline void handle_arp_rep(struct control_context *ctx,
  struct queue_entry *qe)
{
  int i, ret;
  struct queue_entry *arp_up;
  struct arp_entry *ae;
  struct queue_arp_rx_rep *arp_rep = &qe->data.arp_pkt_rx_rep;
  
  /* Check if this ARP reply is for us and is pending */
  ae = arp_lookup(&ctx->arp_table, arp_rep->spa);
  if (ae == NULL || !ae->pending)
    return;
  
  ae = arp_insert(&ctx->arp_table, arp_rep->spa, (__u8 *) &arp_rep->sha);
  if (ae == NULL)
    LOG_ERROR("ARP table full");
    
  /* Cancel timeout */
  if (ae->te != NULL)
  {
    tomgr_cancel(ctx->tomgr, ae->te);
    ae->te = NULL;
  }

  /* Send message to each fast-path to update their ARP tables */
  for (i = 0; i < ctx->config->fp_cores_max; i++)
  {
    arp_up = queue_tail(ctx->ctl_fast_qs[i]);
    if (arp_up == NULL)
    {
      LOG_ERROR("failed to get tail of control->fast queue");
      return;
    }
    
    arp_up->data.arp_update.ip = arp_rep->spa;
    memcpy(&arp_up->data.arp_update.mac, &arp_rep->sha, ETH_ADDR_LEN);
    
    ret = queue_enqueue(ctx->ctl_fast_qs[i], QUEUE_ARP_UPDATE);
    if (ret != 0)
    {
      LOG_ERROR("failed to enqueue ARP update to control->fast queue");
      return;
    }
  }
    
  return;
}

static inline void handle_new_queue_req(struct control_context *ctx,
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
  res = (struct queue_new_queue_res *)&qe_res->data;

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
  
  return;
}

static inline void handle_new_map_req(struct control_context *ctx,
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
  return;
}

static inline void handle_enableq_req(struct control_context *ctx,
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
  req_fast->qid = req->qid;
  req_fast->off = pq->off;
  req_fast->nelems = pq->nelems;
  req_fast->elsize = pq->elsize;
  req_fast->core = req->core;
  g->proto.queues[req->qid].core = req->core;

  /* Enable queue in fast-path */
  ret = queue_enqueue(q, QUEUE_ENABLEQ_REQ);
  assert(ret == 0);

  return;
}

static inline void handle_disableq_req(struct control_context *ctx,
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

  return;
}

static inline void handle_allocate_ebpf_req(struct guest_control *g, 
    struct queue_entry *qe_req)
{
  int ret;
  struct queue_allocate_ebpf_req *req;
  struct queue_entry *qe_res;
  struct queue_allocate_ebpf_res *res;
  
  req = &qe_req->data.alloc_ebpf_req;
  qe_res = queue_tail(g->cham_guest_q);
  assert(qe_res != NULL);
  res = (struct queue_allocate_ebpf_res *)&qe_res->data;

  ret = shmalloc_alloc(g->alloc, req->size, &g->ebpf_shm_handle);
  if (ret != 0)
  {
    LOG_ERROR("failed to allocate memory for eBPF program");
    res->size = 0;
    res->off = 0;
    res->opaque = req->opaque;
    ret = queue_enqueue(g->cham_guest_q, QUEUE_ALLOCATE_EBPF_RES);
    assert(ret == 0);
    return;
  }
  
  memset(g->ebpf_shm_handle->addr, 0, g->ebpf_shm_handle->len);
  res->size = req->size;
  res->off = g->ebpf_shm_handle->off;
  res->opaque = req->opaque;
  ret = queue_enqueue(g->cham_guest_q, QUEUE_ALLOCATE_EBPF_RES);
  assert(ret == 0);
  
  return;
}

static inline void handle_upload_ebpf_req(struct control_context *ctx,
    struct guest_control *g, struct queue_entry *qe_req)
{
  int ret, i;
  void *ebpf_bytecode;
  struct queue_up_ebpf_req *req, *f_req;
  struct queue_up_ebpf_res *res;
  struct queue_entry *qe_res;
  struct bpf_object *bpf_obj;
  const void *event_rx_insns, *event_tx_insns, *event_deq_insns;
  struct bpf_program *event_rx_prog, *event_tx_prog, *event_deq_prog;
  struct ebpf_vm_c *event_rx_vm, *event_tx_vm, *event_deq_vm;

  req = &qe_req->data.up_ebpf_req;

  qe_res = queue_tail(g->cham_guest_q);
  assert(qe_res != NULL);
  res = (struct queue_up_ebpf_res *)&qe_res->data;
  
  ebpf_bytecode = (__u8 *)g->shm_base + req->off;
  bpf_obj = bpf_object__open_mem(ebpf_bytecode, req->size, NULL);
  if (bpf_obj == NULL)
  {
    LOG_ERROR("failed to open bpf_obj from bytecode");
    res->success = 0;
    ret = queue_enqueue(g->cham_guest_q, QUEUE_UPLOAD_EBPF_RES);
    assert(ret == 0);
    return;
  }
  
  /* Verify and JIT RX snippet */
  event_rx_prog = bpf_object__find_program_by_name(bpf_obj, "event_rx");
  if (event_rx_prog == NULL)
  {
    LOG_ERROR("failed to get event_rx from bpf_obj");
    res->success = 0;
    ret = queue_enqueue(g->cham_guest_q, QUEUE_UPLOAD_EBPF_RES);
    assert(ret == 0);
    return;
  }
  
  event_rx_insns = bpf_program__insns(event_rx_prog); 
  ret = verifier_analyze(event_rx_insns, bpf_program__insn_cnt(event_rx_prog), 
      ctx->config->shm_len, "event_rx");
  if (ret != 0)
  {
    LOG_ERROR("failed to verify event_rx");
    return;
  }
  LOG_DEBUG("Passed RX verification");
  
  event_rx_vm = jit_ebpf(event_rx_insns, 
      bpf_program__insn_cnt(event_rx_prog) * 8);
  if (event_rx_vm == NULL)
  {
    LOG_ERROR("failed to jit event_rx");
    res->success = 0;
    ret = queue_enqueue(g->cham_guest_q, QUEUE_UPLOAD_EBPF_RES);
    assert(ret == 0);
    return;
  }
  
  /* Verify and JIT TX snippet */
  event_tx_prog = bpf_object__find_program_by_name(bpf_obj, "event_tx");
  if (event_rx_prog == NULL)
  {
    LOG_ERROR("failed to get event_tx from bpf_obj");
    res->success = 0;
    ret = queue_enqueue(g->cham_guest_q, QUEUE_UPLOAD_EBPF_RES);
    assert(ret == 0);
    return;
  }
  
  event_tx_insns = bpf_program__insns(event_tx_prog);
  ret = verifier_analyze(event_tx_insns, bpf_program__insn_cnt(event_tx_prog), 
      ctx->config->shm_len, "event_tx");
  if (ret != 0)
  {
    LOG_ERROR("failed to verify event_tx");
    return;
  }
  LOG_DEBUG("Passed TX verification");
  
  event_tx_vm = jit_ebpf(event_tx_insns, 
      bpf_program__insn_cnt(event_tx_prog) * 8);
  if (event_tx_vm == NULL)
  {
    LOG_ERROR("failed to jit event_tx");
    res->success = 0;
    ret = queue_enqueue(g->cham_guest_q, QUEUE_UPLOAD_EBPF_RES);
    assert(ret == 0);
    return;
  }
  
  /* Verify and JIT DEQ snippet */
  event_deq_prog = bpf_object__find_program_by_name(bpf_obj, "event_deq");
  if (event_rx_prog == NULL)
  {
    LOG_ERROR("failed to get event_deq from bpf_obj");
    res->success = 0;
    ret = queue_enqueue(g->cham_guest_q, QUEUE_UPLOAD_EBPF_RES);
    assert(ret == 0);
    return;
  }
  
  event_deq_insns = bpf_program__insns(event_deq_prog);
  ret = verifier_analyze(event_deq_insns, bpf_program__insn_cnt(event_deq_prog), 
      ctx->config->shm_len, "event_deq");
  if (ret != 0)
  {
    LOG_ERROR("failed to verify event_deq");
    return;
  }
  LOG_DEBUG("Passed DEQ verification");
  event_deq_vm = jit_ebpf(event_deq_insns, 
      bpf_program__insn_cnt(event_deq_prog) * 8);
  if (event_deq_vm == NULL)
  {
    LOG_ERROR("failed to jit event_deq");
    res->success = 0;
    ret = queue_enqueue(g->cham_guest_q, QUEUE_UPLOAD_EBPF_RES);
    assert(ret == 0);
    return;
  }

  /* Send jitted VMs for functions to fast-path  */
  for (i = 0; i < ctx->config->fp_cores_max; i++)
  {
    qe_req = queue_tail(ctx->ctl_fast_qs[i]);
    assert(qe_req != NULL);
    f_req = &qe_req->data.up_ebpf_req;
    f_req->gid = g->id;
    f_req->size = req->size;
    f_req->off = req->off;
    f_req->event_rx_vm = event_rx_vm;
    f_req->event_tx_vm = event_tx_vm;
    f_req->event_deq_vm = event_deq_vm;
    ret = queue_enqueue(ctx->ctl_fast_qs[i], QUEUE_UPLOAD_EBPF_REQ);
    assert(ret == 0);
  }

  res = (struct queue_up_ebpf_res *)&qe_res->data;
  res->success = 1;
  ret = queue_enqueue(g->cham_guest_q, QUEUE_UPLOAD_EBPF_RES);
  assert(ret == 0);
  return;
}

static inline void handle_free_ebpf_req(struct guest_control *g, 
    struct queue_entry *qe_req)
{
  int ret;
  struct queue_free_ebpf_res *res;
  struct queue_entry *qe_res; 

  qe_res = queue_tail(g->cham_guest_q);
  assert(qe_res != NULL);
  
  res = (struct queue_free_ebpf_res *)&qe_res->data;

  shmalloc_free(g->alloc, g->ebpf_shm_handle);

  res->success = 1;
  ret = queue_enqueue(g->cham_guest_q, QUEUE_FREE_EBPF_RES);
  assert(ret == 0);
  return;
}

/* Pointer to the memory with the jitted code inside 
   the ebpf_vm_c struct: ebpf_jitted_fn */
static inline struct ebpf_vm_c * jit_ebpf(const void *ebpf_instrs, size_t size)
{
  __u64 res;
  struct ebpf_vm_c *vm;
  vm = ebpf_vm_create();

  if (vm == NULL)
  {
    LOG_ERROR("failed to create llvmbpf vm");
    return NULL;
  }

  res = ebpf_vm_load_code(vm, ebpf_instrs, size);
  if (res != 0)
  {
    LOG_ERROR("failed to load ebpf bytecode");
    return NULL;
  }

  // Register helper functions here
  res = ebpf_vm_register_helper(vm, 1001, "ebpf_queue_tail", ebpf_queue_tail);
  if (res != 0)
  {
    LOG_ERROR("failed to register ebpf_queue_tail helper");
    return NULL;
  }
  
  res = ebpf_vm_register_helper(vm, 1002, "queue_enqueue", queue_enqueue);
  if (res != 0)
  {
    LOG_ERROR("failed to register ebpf_queue_enqueue helper");
    return NULL;  
  }
  
  res = ebpf_vm_register_helper(vm, 1003, "ebpf_memcpy", ebpf_memcpy);
  if (res != 0)
  {
    LOG_ERROR("failed to register ebpf_memcpy helper");
    return NULL;
  }

  res = ebpf_vm_register_helper(vm, 1004, "ebpf_print", ebpf_print);
  if (res != 0)
  {
    LOG_ERROR("failed to register ebpf_print helper");
    return NULL;
  }

  res = ebpf_vm_register_helper(vm, 1005, "ebpf_ipv4_checksum", ebpf_ipv4_checksum);
  if (res != 0)
  {
    LOG_ERROR("failed to register ebpf_ipv4_checksum helper");
    return NULL;
  }
  
  res = ebpf_vm_register_helper(vm, 1006, "ebpf_ipv4_udptcp_cksum", ebpf_ipv4_udptcp_cksum);
  if (res != 0)
  {
    LOG_ERROR("failed to register ebpf_ipv4_udptcp_cksum helper");
    return NULL;
  }
  
  res = ebpf_vm_register_helper(vm, 1007, "sched_head", sched_head);
  if (res != 0)
  {
    LOG_ERROR("failed to register sched_head helper");
    return NULL;
  }
  
  res = ebpf_vm_register_helper(vm, 1008, "sched_pop", sched_pop);
  if (res != 0)
  {
    LOG_ERROR("failed to register sched_pop helper");
    return NULL;
  }
  
  res = ebpf_vm_register_helper(vm, 1009, "sched_add", sched_add);
  if (res != 0)
  {
    LOG_ERROR("failed to register sched_add helper");
    return NULL;
  }

  res = ebpf_vm_register_helper(vm, 1010, "ebpf_map_get", ebpf_map_get);
  if (res != 0)
  {
    LOG_ERROR("failed to register ebpf_map_get helper");
    return NULL;
  }

  res = ebpf_vm_register_helper(vm, 1011, "ebpf_map_lookup", ebpf_map_lookup);
  if (res != 0)
  {
    LOG_ERROR("failed to register ebpf_map_lookup helper");
    return NULL;
  }
  
  res = ebpf_vm_compile(vm);
  if (res != 0)
  {
    LOG_ERROR("failed to JIT ebpf bytecode");
    return NULL;
  }
  
  return vm;
}

static void ebpf_print(int a)
{
  LOG_DEBUG("HERE %lld", a);
}

static inline void * ebpf_memcpy(void *dst, void *src, size_t n)
{
  return memcpy(dst, src, n);
}

static inline __u16 ebpf_ipv4_checksum(void *ip_hdr)
{
  return rte_ipv4_cksum(ip_hdr);
}

static inline __u16 ebpf_ipv4_udptcp_cksum(void *ip_hdr, void *udp_hdr)
{
  return rte_ipv4_udptcp_cksum(ip_hdr, udp_hdr);
}

static inline void * ebpf_map_get(void *map_base, __u32 len)
{
  return map_base;
}

static inline void * ebpf_map_lookup(void *map_base, __u64 id, __u64 elsize)
{
  return map_base + (id * elsize);
}

static inline void * ebpf_queue_tail(struct equeue *q, __u64 elsize)
{
  return queue_tail(q);
}


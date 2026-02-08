#include <stdlib.h>
#include <linux/types.h>
#include <assert.h>
#include <string.h>

#include "control.h"
#include "tomgr.h"
#include "clock.h"
#include "nic.h"
#include "ivshmemif.h"
#include "guestif.h"
#include "config.h"
#include "log.h"
#include "queue_fns.h"
#include "queue_types.h"
#include "control_arp.h"
#include "control_budget.h"
#include "control_ebpf.h"
#include "control_guest.h"

static inline int poll_fast(struct control_context *ctx);
static inline int poll_guests(struct control_context *ctx);
static inline int poll_timeouts(struct control_context *ctx);

int control_context_init(struct control_context *ctrl_ctx, 
    struct nic_context *nic_ctx, struct configuration *config, 
    struct shm_handle **fc_handles, struct shm_handle **cf_handles,
    struct shm_handle **txq_handles)
{
  int i;
  struct tomgr *tomgr;
  struct equeue *cfq, *txq;
  struct dqueue *fcq;
  struct dqueue **fast_ctl_qs;
  struct equeue **ctl_fast_qs, **txqs;

  ctrl_ctx->config = config;
  ctrl_ctx->nic_ctx = nic_ctx;
  ctrl_ctx->comb_bc.data = NULL;
  ctrl_ctx->comb_bc.len = 0;

  if (control_ebpf_init(ctrl_ctx) != 0)
  {
    LOG_ERROR("failed to build infra bytecode");
    return -1;
  }

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

  ctrl_ctx->n_guests = 0;
  ctrl_ctx->next_guest = 0;
  if (config->perf_iso)
  {
    ctrl_ctx->ts_refresh = clock_rdtsc();
    ctrl_ctx->budget_cap = clock_us_to_tsc(config->perf_iso_cap) *
        config->perf_iso_boost;
  }

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
    control_budget_refresh(ctx);
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
  while (i < CONTROL_BATCH_SIZE)
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
        control_arp_lookup(ctx, qe);
        queue_dequeue(q);
        break;
      case QUEUE_ARP_RX_REQ:
        control_arp_req(ctx, qe);
        queue_dequeue(q);
        break;
      case QUEUE_ARP_RX_REP:
        control_arp_rep(ctx, qe);
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
  while (i < CONTROL_BATCH_SIZE)
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
      case QUEUE_NEW_PROTO_REQ:
        control_guest_new_proto(ctx, g, qe);
        queue_dequeue(q);
        break;
      case QUEUE_NEW_QUEUE_REQ:
        control_guest_new_queue(ctx, g, qe);
        queue_dequeue(q);
        break;
      case QUEUE_NEW_MAP_REQ:
        control_guest_new_map(ctx, g, qe);
        queue_dequeue(q);
        break;
      case QUEUE_ENABLEQ_REQ:
        control_guest_enableq(ctx, g, qe);
        queue_dequeue(q);
        break;
      case QUEUE_DISABLEQ_REQ:
        control_guest_disableq(ctx, g, qe);
        queue_dequeue(q);
        break;
      case QUEUE_ALLOCATE_EBPF_REQ:
        control_ebpf_allocate(g, qe);
        queue_dequeue(q);
        break;
      case QUEUE_FREE_EBPF_REQ:
        control_ebpf_free(g, qe);
        queue_dequeue(q);
        break;
      case QUEUE_UPLOAD_EBPF_REQ:
        control_ebpf_upload(ctx, g, qe);
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
  
  for (i = 0; i < CONTROL_BATCH_SIZE; i++)
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
        control_arp_timeout(ctx, te);
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

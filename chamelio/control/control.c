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
static inline void log_fast_batch_stats(struct control_context *ctx);

int control_context_init(struct control_context *ctl_ctx, 
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

  ctl_ctx->config = config;
  ctl_ctx->nic_ctx = nic_ctx;
  ctl_ctx->comb_bc.data = NULL;
  ctl_ctx->comb_bc.len = 0;
  ctl_ctx->comb_helpers_bc.data = NULL;
  ctl_ctx->comb_helpers_bc.len = 0;
  memset(ctl_ctx->guest_bc, 0, sizeof(ctl_ctx->guest_bc));
  ctl_ctx->agg_rx_vm = NULL;
  ctl_ctx->agg_deq_vm = NULL;
  ctl_ctx->agg_tx_vm = NULL;
  ctl_ctx->f_ctxs = NULL;
  ctl_ctx->fast_batch_last = NULL;
  ctl_ctx->fast_stats_tsc = 0;
#if CHAM_CTL_BUDGET_STATS
  memset(&ctl_ctx->budg_stats, 0, sizeof(ctl_ctx->budg_stats));
  memset(&ctl_ctx->budg_last, 0, sizeof(ctl_ctx->budg_last));
#endif

  if (control_ebpf_init(ctl_ctx) != 0)
  {
    LOG_ERROR("failed to build infra bytecode");
    return -1;
  }

  ctl_ctx->ivshmem_uxfd = -1;
  ctl_ctx->ivshmem_epfd = -1;

  ctl_ctx->guest_uxfd = -1;
  ctl_ctx->guest_epfd = -1;

  /* Initialize ARP table to default values */
  arp_table_init(&ctl_ctx->arp_table);
  
  /* Initialize timeout manager */
  tomgr = tomgr_init();
  if (tomgr == NULL)
  {
    LOG_ERROR("failed to initialise timeout manager");
    return -1;
  }
  ctl_ctx->tomgr = tomgr;

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
  ctl_ctx->fast_ctl_qs = fast_ctl_qs;
  ctl_ctx->next_core = 0;

  /* Allocate pointer list for queues from control to fast */
  ctl_fast_qs = malloc(sizeof(struct equeue *) * config->fp_cores_max);
  if (ctl_fast_qs == NULL)
  {
    LOG_ERROR("failed to alloacate list of control->fast queues");
    goto free_fast_control_list;
  }
  ctl_ctx->ctl_fast_qs = ctl_fast_qs;

  /* Allocate pointer list for queues containing packets for transmission */
  txqs = malloc(sizeof (struct equeue *) * config->fp_cores_max);
  if (txqs == NULL)
  {
    LOG_ERROR("failed to allocate list of control-path transmit queues");
    goto free_control_fast_list;
  }
  ctl_ctx->txqs = txqs;
  ctl_ctx->fast_batch_last = calloc(config->fp_cores_max,
      sizeof(*ctl_ctx->fast_batch_last));
  if (ctl_ctx->fast_batch_last == NULL)
  {
    LOG_ERROR("failed to allocate fast-path batch stat snapshots");
    goto free_control_txqs;
  }
  
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
    ctl_ctx->ctl_fast_qs[i] = cfq;

    fcq = dqueue_new(config->cham_queue_len, sizeof(struct queue_entry),
                     fc_handles[i]->addr, fc_handles[i]->off);
    if (fcq == NULL)
    {
      LOG_ERROR("failed to create control to fast path queue");
      goto free_control_txqs;
    }
    ctl_ctx->fast_ctl_qs[i] = fcq;
    
    txq = equeue_new(config->control_txq_len, config->control_txq_pkt_len, 
        txq_handles[i]->addr, txq_handles[i]->off);
    if (txq == NULL)
    {
      LOG_ERROR("failed to create control path transmit queue");
      goto free_control_txqs;
    }
    ctl_ctx->txqs[i] = txq;
  }

  ctl_ctx->n_guests = 0;
  ctl_ctx->next_guest = 0;
  if (config->perf_iso)
  {
    ctl_ctx->ts_refresh = clock_rdtsc();
    ctl_ctx->budget_cap = clock_us_to_tsc(config->perf_iso_cap);
  }
  ctl_ctx->fast_stats_tsc = clock_rdtsc();

  if (config->fp_jit_combined && control_ebpf_publish(ctl_ctx) != 0)
  {
    LOG_ERROR("failed to publish aggregate combined entries");
    goto free_control_txqs;
  }

  return 0;

free_control_txqs:
  free(ctl_ctx->fast_batch_last);
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
    log_fast_batch_stats(ctx);
  }
}

static inline void log_fast_batch_stats(struct control_context *ctx)
{
  __u16 i;
  __u64 now_tsc;
  __u64 rx_calls;
  __u64 queue_calls;
  __u64 tx_calls;
#if CHAM_CTL_BUDGET_STATS
  __u8 gid;
  __u64 budg_nr;
  __u64 avg_gap;
  __u64 avg_core;
  __u64 avg_guest;
  struct ctl_budg_stats budg_cur;
#endif
  struct fast_context *f_ctx;
  struct fast_batch_counters cur;
  struct fast_batch_counters *prev;

  if (ctx->f_ctxs == NULL || ctx->fast_batch_last == NULL)
    return;

  now_tsc = clock_rdtsc();
  if (ctx->fast_stats_tsc != 0 &&
      clock_us_since_tsc(ctx->fast_stats_tsc) < 1000000)
  {
    return;
  }
  ctx->fast_stats_tsc = now_tsc;

#if CHAM_CTL_BUDGET_STATS
  budg_nr = 0;
  if (ctx->config->perf_iso)
  {
    budg_cur = ctx->budg_stats;
    budg_nr = budg_cur.nr - ctx->budg_last.nr;
    avg_gap = budg_nr == 0 ? 0 : (budg_cur.cyc - ctx->budg_last.cyc) / budg_nr;
    LOG_INFO("budg refresh_avg=%llu cyc [%llu us] nr=%llu",
        (unsigned long long) avg_gap,
        (unsigned long long) clock_tsc_to_us(avg_gap),
        (unsigned long long) budg_nr);
    ctx->budg_last = budg_cur;
  }
#endif

  for (i = 0; i < ctx->config->fp_cores_max; i++)
  {
    f_ctx = ctx->f_ctxs[i];
    if (f_ctx == NULL)
      continue;

    if (fast_batch_stats_snapshot(f_ctx, &cur) != 0)
      continue;
    prev = &ctx->fast_batch_last[i];
    rx_calls = cur.rx_calls - prev->rx_calls;
    queue_calls = cur.queue_calls - prev->queue_calls;
    tx_calls = cur.tx_calls - prev->tx_calls;

    LOG_INFO("fast_batch core=%u fast_rx_poll_avg=%.2f fast_queues_poll_avg=%.2f "
        "fast_tx_poll_avg=%.2f",
        i,
        rx_calls == 0 ? 0.0 : (double) (cur.rx_items - prev->rx_items) / rx_calls,
        queue_calls == 0 ? 0.0 :
            (double) (cur.queue_items - prev->queue_items) / queue_calls,
        tx_calls == 0 ? 0.0 : (double) (cur.tx_items - prev->tx_items) / tx_calls);

#if CHAM_CTL_BUDGET_STATS
    if (ctx->config->perf_iso)
    {
      avg_core = budg_nr == 0 ? 0 : (cur.budg_cyc - prev->budg_cyc) / budg_nr;
      LOG_INFO("budg core=%u avg=%llu cyc [%llu us]",
          i,
          (unsigned long long) avg_core,
          (unsigned long long) clock_tsc_to_us(avg_core));

      for (gid = 0; gid < ctx->n_guests; gid++)
      {
        avg_guest = budg_nr == 0 ? 0 :
            (cur.guest_budg_cyc[gid] - prev->guest_budg_cyc[gid]) / budg_nr;
        LOG_INFO("budg core=%u gid=%u avg=%llu cyc [%llu us]",
            i, gid,
            (unsigned long long) avg_guest,
            (unsigned long long) clock_tsc_to_us(avg_guest));
      }
    }
#endif

    *prev = cur;
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

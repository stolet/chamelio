#ifndef FAST_STATS_H_
#define FAST_STATS_H_

#include "config.h"
#include <linux/types.h>

#define CHAM_CTL_BUDGET_STATS 0
#define CHAM_CTL_CYCLES_STATS 1

struct fast_context;

struct fast_stats {
  __u64 rx_calls;
  __u64 rx_items;
  __u64 queue_calls;
  __u64 queue_items;
  __u64 sched_calls;
  __u64 sched_items;
#if CHAM_CTL_CYCLES_STATS
  __u64 rx_cyc;
  __u64 queue_cyc;
  __u64 sched_cyc;
#endif
#if CHAM_CTL_BUDGET_STATS
  __u64 budg_cyc;
  __u64 guest_budg_cyc[CHAMELIO_MAX_GUESTS];
#endif
};

int fast_stats_snapshot(struct fast_context *ctx,
    struct fast_stats *stats);

#endif

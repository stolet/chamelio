#ifndef FAST_STATS_H_
#define FAST_STATS_H_

#include "config.h"
#include <linux/types.h>

#ifndef CHAM_CTL_BUDGET_STATS
#define CHAM_CTL_BUDGET_STATS 0
#endif

struct fast_context;

struct fast_batch_counters {
  __u64 rx_calls;
  __u64 rx_items;
  __u64 queue_calls;
  __u64 queue_items;
  __u64 tx_calls;
  __u64 tx_items;
#if CHAM_CTL_BUDGET_STATS
  __u64 budg_cyc;
  __u64 guest_budg_cyc[CHAMELIO_MAX_GUESTS];
#endif
};

#if CHAM_CTL_BUDGET_STATS
#define fast_budg_add(_ctx, _gid, _cyc)                                       \
  do {                                                                        \
    (_ctx)->batch_stats.budg_cyc += (_cyc);                                   \
    (_ctx)->batch_stats.guest_budg_cyc[_gid] += (_cyc);                       \
  } while (0)
#else
#define fast_budg_add(_ctx, _gid, _cyc) do { } while (0)
#endif

int fast_batch_stats_snapshot(const struct fast_context *ctx,
    struct fast_batch_counters *stats);

#endif

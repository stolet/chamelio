#ifndef FAST_STATS_H_
#define FAST_STATS_H_

#include <linux/types.h>

struct fast_context;

struct fast_batch_counters {
  __u64 rx_calls;
  __u64 rx_items;
  __u64 queue_calls;
  __u64 queue_items;
  __u64 tx_calls;
  __u64 tx_items;
};

int fast_batch_stats_snapshot(const struct fast_context *ctx,
    struct fast_batch_counters *stats);

#endif

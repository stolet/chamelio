#ifndef FAST_COMB_H_
#define FAST_COMB_H_

#include <linux/types.h>

struct fast_context;
struct guest_fast;
struct queue_entry;
struct rte_mbuf;

/* Context for combined JIT of RX event. */
struct fast_comb_rx_ctx {
  /* Guest VM that uploaded the combined snipped for this ctx */
  struct guest_fast *g;
  /* Mbuf containing packet data */
  struct rte_mbuf *mb;
  /* Offset to start of mbuf data after infra processing */
  __u64 pkt_off;
};

/* Context for combined JIT of dequeue event. */
struct fast_comb_deq_ctx {
  /* Context for this fast-path core */
  struct fast_context *f_ctx;
  /* Guest VM that uploaded the combined snipped for this ctx */
  struct guest_fast *g;
  /* Mbuf containing packet data */
  struct rte_mbuf *mb;
  /* Dequeued queue entry */
  struct queue_entry *qe;
  /* ID of dequeued queue */
  __u16 qid;
};

/* Context for combined JIT of TX event. */
struct fast_comb_tx_ctx {
  /* Context for this fast-path core */
  struct fast_context *f_ctx;
  /* Guest VM that uploaded the combined snipped for this ctx */
  struct guest_fast *g;
  /* Mbuf containing packet data */
  struct rte_mbuf *mb;
};

#endif

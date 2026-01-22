#ifndef FAST_JIT_H_
#define FAST_JIT_H_

#include <linux/types.h>

struct fast_context;
struct guest_fast;
struct queue_entry;
struct rte_mbuf;

/* Context for combined JIT of RX event. */
struct fast_jit_rx_ctx {
  struct guest_fast *g;
  struct rte_mbuf *mb;
  __u64 pkt_off;
};

/* Context for combined JIT of dequeue event. */
struct fast_jit_deq_ctx {
  struct fast_context *f_ctx;
  struct guest_fast *g;
  struct rte_mbuf *mb;
  struct queue_entry *qe;
  __u16 qid;
};

/* Context for combined JIT of TX event. */
struct fast_jit_tx_ctx {
  struct fast_context *f_ctx;
  struct guest_fast *g;
  struct rte_mbuf *mb;
};

#endif

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
  /* 1 if the tenant path uses outer IP + GRE encapsulation */
  __u8 virt_gre;
};

/* Context for combined JIT of dequeue event. */
struct fast_comb_deq_ctx {
  /* Context for this fast-path core */
  struct fast_context *f_ctx;
  /* Guest VM that uploaded the combined snipped for this ctx */
  struct guest_fast *g;
  /* Mbufs containing packet data */
  struct rte_mbuf **mbs;
  /* Max number of mbufs to process */
  int max;
  /* In/out count of mbufs processed */
  int *ntx;
  /* In/out count of dequeued entries */
  int *ndeq;
};

/* Context for combined JIT of TX event. */
struct fast_comb_tx_ctx {
  /* Context for this fast-path core */
  struct fast_context *f_ctx;
  /* Guest VM that uploaded the combined snipped for this ctx */
  struct guest_fast *g;
  /* Mbufs containing packet data */
  struct rte_mbuf **mbs;
  /* Max number of mbufs to process */
  int max;
  /* In/out count of mbufs processed */
  int *ntx;
};

#endif

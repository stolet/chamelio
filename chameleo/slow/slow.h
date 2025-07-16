#ifndef SLOW_H_
#define SLOW_H_

// #include "routing/routing.h"

struct slow_context {
  /* Number of fast-path cores */
  int n_cores;
  /* Routing table */
  // struct routing_table rt;
  /* Queues from the fast-path to slow-path. One per core. */
  struct queue **fast_slow_qs;
  /* Queues from the slow-path to the fast-path. One per core */
  struct queue **slow_fast_qs;
};

int slow_context_init(struct slow_context *s_ctx, uint16_t n_cores,
    struct queue **fast_slow_qs, struct queue **slow_fast_qs);
int slow_loop(struct slow_context *ctx);


#endif
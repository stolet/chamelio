#ifndef SLOW_H_
#define SLOW_H_

#include "config.h"

struct slow_context {
  /* Configuration parameters */
  struct configuration *config;
  /* Queues from the fast-path to slow-path. One per core. */
  struct queue **fast_slow_qs;
  /* Queues from the slow-path to the fast-path. One per core */
  struct queue **slow_fast_qs;

  /* Listening UX sockets for guests */
  int guest_uxfd;
  /* Epoll object used by UX guest socket */
  int guest_epfd;
  /* Next unused VM ID */
  int guest_id_next;

  /* Listening UX sockets for apps */
  int app_uxfd;
  /* Epoll object used by UX app socket */
  int app_epfd;
  /* Next unused application id */
  int app_id_next;

};

int slow_context_init(struct slow_context *s_ctx, struct configuration *config,
    struct queue **fast_slow_qs, struct queue **slow_fast_qs);
int slow_loop(struct slow_context *ctx);


#endif
#ifndef UDP_SLOW_H_
#define UDP_SLOW_H_

#include <cham_lib.h>

#include "udp.h"

struct udp_app_context_slow {
  /* ID for application context */
  uint8_t id;
  /* Application this context belongs to */
  struct udp_app_slow *app;
  /* Queue for messages app->slow */
  struct dqueue *app_slow_q;
  /* Queue for messages slow->app */
  struct equeue *slow_app_q;
  /* App bump queues */
  struct proto_queue_lib *app_bump_qs[MAX_FP_CORES];
  /* Fast-path bump queues */
  struct proto_queue_lib *fast_bump_qs[MAX_FP_CORES];
};

struct udp_app_slow {
  /* ID of the application */
  uint8_t id;
  /* Number of registered application contexts */
  uint8_t n_ctxs;
  /* List of application contexts */
  struct udp_app_context_slow ctxs[MAX_CTXS];
  /* Map of offsets to different Chamelio maps */
  struct proto_map_lib *offs_map;
};

struct udp_slow_context {
  /* Listening UX sockets for new applications */
  int app_uxfd;
  /* Epoll object used by UX application socket */
  int app_epfd;

  /* Chamelio library guest structure */
  struct guest_lib *guest;
  /* Chamelio library protocol structure */
  struct proto_lib *proto;

  /* Number of registered applications */
  uint8_t n_apps;
  /* Apps that have registered with chamelio */
  struct udp_app_slow apps[MAX_APPS];
};

#endif
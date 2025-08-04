#ifndef SLOW_H_
#define SLOW_H_

#include "config.h"
#include "shmalloc.h"

struct app_context_slow {
  /* Id of this application context */
  uint8_t id;
  /* Application that owns this context */
  struct app_slow *app;

  /* Queue from the application context to the Chamelio slow-path */
  struct dqueue *app_cham_q;
  /* Queue from the Chamelio slow-path to the application context */
  struct equeue *cham_app_q;

  /* List of rx queue handles for each app context and fast-path core */
  struct shm_handle **rxq;
  /* List of tx queue handles for each app context and fast-path core */
  struct shm_handle **txq;
};

struct app_slow {
  /* Id of the registered application */
  uint8_t id;
  /* Protocol of the registered application */
  uint8_t proto_type;
  /* Guest where this application is running */
  struct guest_slow *guest;

  /* Number of contexts for this app */
  uint8_t n_ctxs;
  /* Application contexts. One per app core. */
  struct app_context_slow *ctxs;
};

struct guest_slow {
  /* ID of registered guest */
  uint8_t id;

  /* File descriptor for shared memory region for this guest */
  int shm_fd;
  /* Base pointer to the shared memory region for this guest */
  void *shm_base;
  /* Allocator for shared memory region */
  struct shm_allocator *alloc;
  
  /* Queue from the guest agent to the Chamelio slow-path */
  struct dqueue *agt_cham_q;
  /* Queue from the Chamelio slow-path to the guest agent */
  struct equeue *cham_agt_q;
  
  /* Number of registered applications */
  uint8_t n_apps;
  /* Applications registered in this guest */
  struct app_slow *apps;
};

struct slow_context {
  /* Configuration parameters */
  struct configuration *config;
  /* Queues from the fast-path to slow-path. One per core. */
  struct dqueue **fast_slow_qs;
  /* Queues from the slow-path to the fast-path. One per core */
  struct equeue **slow_fast_qs;

  /* File descriptor for internal shared memory */
  int shm_fd_internal;
  /* Base pointer for internal shared memory region */
  void *shm_base_internal;

  /* Listening UX sockets for guests */
  int guest_uxfd;
  /* Epoll object used by UX guest socket */
  int guest_epfd;

  /* Listening UX sockets for apps */
  int app_uxfd;
  /* Epoll object used by UX app socket */
  int app_epfd;

  /* Number of registered guests */
  uint8_t n_guests;
  /* Guests that have registered with chamelio */
  struct guest_slow *guests;
};

int slow_context_init(struct slow_context *s_ctx, struct configuration *config,
    struct shm_handle **fs_handles, struct shm_handle **sf_handles);
int slow_loop(struct slow_context *ctx);

#endif
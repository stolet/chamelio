#ifndef SLOW_H_
#define SLOW_H_

#include "config.h"
#include "shmalloc.h"

struct app_context {
  /* Id of this application context */
  uint8_t id;
  /* Application that owns this context */
  struct app_slow *app;

  /* Queue from the application to the Chamelio slow-path */
  struct queue *app_cham_q;
  /* Queue from the Chamelio slow-path to the application */
  struct queue *cham_app_q;

  /* List of rx queues for each application context and a fast-path core */
  struct shm_handle **rxq;
  /* List of tx queues for each application context and a fast-path core */
  struct shm_handle **txq;

  /* Next app context in the list */
  struct app_context *next;
};

struct app_slow {
  /* Id of the registered application */
  uint8_t id;
  /* Guest where this application is running */
  struct guest_slow *guest;
  /* Contexts for this application */
  struct app_context *ctxs;
  /* Next application in the list */
  struct app_slow *next;
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
  
  /* Queue from the guest to the Chamelio slow-path */
  struct queue *guest_cham_q;
  /* Queue from the Chamelio slow-path to the guest */
  struct queue *cham_guest_q;
  
  /* Applications registered in this guest */
  struct app_slow *apps;

  /* Next guest in the list */
  struct guest_slow *next;
};

struct slow_context {
  /* Configuration parameters */
  struct configuration *config;
  /* Queues from the fast-path to slow-path. One per core. */
  struct queue **fast_slow_qs;
  /* Queues from the slow-path to the fast-path. One per core */
  struct queue **slow_fast_qs;

  /* File descriptor for internal shared memory */
  int shm_fd_internal;
  /* Base pointer for internal shared memory region */
  void *shm_base_internal;

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

  /* Guests that have registered with chamelio */
  struct guest_slow *guests;
};

int slow_context_init(struct slow_context *s_ctx, struct configuration *config,
    struct queue **fast_slow_qs, struct queue **slow_fast_qs);
int slow_loop(struct slow_context *ctx);

#endif
#ifndef CONTROL_H_
#define CONTROL_H_

#include "config.h"
#include "queue.h"
#include "shmalloc.h"

#define CORE_INVALID UINT16_MAX

struct proto_queue_control {
  /* Queue ID */
  uint16_t id;
  /* Offset in shared memory for start of this queue */
  uint64_t off;
  /* Core where this queue is currently running */
  uint16_t core;
};

struct proto_control {
  /* Guest that this protocol belongs to */
  struct guest_control *guest;

  /* Number of queues in shared memory */
  uint16_t nqueues;
  /* Number of elements in each queue */
  uint32_t nelems;
  /* Size of each queue element */
  uint32_t elsize;
  /* List of queues for this protocol */
  struct proto_queue_control queues[MAX_PROTO_QUEUES];

  /* Number of maps created */
  uint16_t nmaps;
};

struct guest_control {
  /* ID of registered guest */
  uint8_t id;

  /* File descriptor for shared memory region for this guest */
  int shm_fd;
  /* Base pointer to the shared memory region for this guest */
  void *shm_base;
  /* Allocator for shared memory region */
  struct shm_allocator *alloc;
  
  /* Queue from the guest to the Chamelio control-path */
  struct dqueue *guest_cham_q;
  /* Queue from the Chamelio control-path to the guest */
  struct equeue *cham_guest_q;
  
  /* Protocol registered for this guest */
  struct proto_control proto;
};

struct control_context {
  /* Configuration parameters */
  struct configuration *config;
  /* Queues from the fast-path to control-path. One per core. */
  struct dqueue **fast_ctl_qs;
  /* Queues from the control-path to the fast-path. One per core */
  struct equeue **ctl_fast_qs;

  /* File descriptor for internal shared memory */
  int shm_fd_internal;
  /* Base pointer for internal shared memory region */
  void *shm_base_internal;

  /* Listening UX sockets for VMs */
  int ivshmem_uxfd;
  /* Epoll object used by UX VM socket */
  int ivshmem_epfd;

  /* Listening UX sockets for guests and protocols */
  int guest_uxfd;
  /* Epoll object used by UX guest socket */
  int guest_epfd;

  /* Number of registered guests */
  uint8_t n_guests;
  /* Guests that have registered with chamelio */
  struct guest_control *guests;
};

int control_context_init(struct control_context *s_ctx, struct configuration *config,
    struct shm_handle **fs_handles, struct shm_handle **sf_handles);
int control_loop(struct control_context *ctx);

#endif
#ifndef CONTROL_H_
#define CONTROL_H_

#include "config.h"
#include "queue.h"
#include "shmalloc.h"

#define CORE_INVALID UINT16_MAX
#define BATCH_SIZE 16

struct proto_queue_control {
  /* Queue ID */
  __u16 id;
  /* Number of elements in the queue */
  __u32 nelems;
  /* Size of elements in the queue */
  __u32 elsize;
  /* Offset in shared memory for start of this queue */
  __u64 off;
  /* Core where this queue is currently running */
  __u16 core;
};

struct proto_control {
  /* Guest that this protocol belongs to */
  struct guest_control *guest;

  /* Number of queues in shared memory */
  __u16 nqueues;
  /* List of queues for this protocol */
  struct proto_queue_control queues[MAX_PROTO_QUEUES];

  /* Number of maps created */
  __u16 nmaps;
};

struct guest_control {
  /* ID of registered guest */
  __u8 id;

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
  /* Next fast-path core to poll */
  __u16 next_core;
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
  __u8 n_guests;
  /* Next guest to poll */
  __u16 next_guest;
  /* Guests that have registered with chamelio */
  struct guest_control *guests;
};

int control_context_init(struct control_context *s_ctx, struct configuration *config,
    struct shm_handle **fs_handles, struct shm_handle **sf_handles);
int control_loop(struct control_context *ctx);

#endif
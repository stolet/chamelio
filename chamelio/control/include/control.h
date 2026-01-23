#ifndef CONTROL_H_
#define CONTROL_H_

#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "queue.h"
#include "arp.h"
#include "shmalloc.h"
#include "tomgr.h"

#define CORE_INVALID UINT16_MAX
#define CONTROL_BATCH_SIZE 16

struct infra_bc_blob {
  void *data;
  size_t len;
};

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

  /* Guest budget counter shared with control */
  __s64 budget;

  /* File descriptor for shared memory region for this guest */
  int shm_fd;
  /* Base pointer to the shared memory region for this guest */
  void *shm_base;
  /* Allocator for shared memory region */
  struct shm_allocator *alloc;
  /* eBPF shared memory handle */
  struct shm_handle *ebpf_shm_handle;
  
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
  /* Infra bytecode blob for combined JIT */
  struct infra_bc_blob infra_bc;
  /* NIC paremeters and configuration */
  struct nic_context *nic_ctx;
  /* Next fast-path core to poll */
  __u16 next_core;
  /* Timeout manager */
  struct tomgr *tomgr;
  /* Timestamp for last budget refresh */
  __u64 ts_refresh;
  /* Budget cap in CPU cycles */
  __u64 budget_cap;
  
  /* Queues from the fast-path to control-path. One per core. */
  struct dqueue **fast_ctl_qs;
  /* Queues from the control-path to the fast-path. One per core */
  struct equeue **ctl_fast_qs;
  /* Queue that pushes packets from control to fast. One per core. */
  struct equeue **txqs;
  
  /* File descriptor for internal shared memory */
  int shm_fd_internal;
  /* Base pointer for internal shared memory region */
  void *shm_base_internal;

  /* ARP table. This is also replicated in fast-path */
  struct arp_table arp_table;
  
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

/* Read-only after being initialized */
  /* Network virtualization table indexed by GRE key and inner IP */
  struct netvirt_table *inner_table;
  /* Network virtualization table indexed by guest ID and outer IP */
  struct netvirt_table *gid_table;
};

int control_context_init(struct control_context *ctrl_ctx, 
    struct nic_context *nic_ctx, struct configuration *config, 
    struct shm_handle **fc_handles, struct shm_handle **cf_handles,
    struct shm_handle **txq_handles);
int control_loop(struct control_context *ctx);

#endif

#ifndef CONTROL_H_
#define CONTROL_H_

#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "cham_fast.h"
#include "ebpf.h"
#include "queue.h"
#include "arp.h"
#include "fast_stats.h"
#include "shmalloc.h"
#include "tomgr.h"

#define CORE_INVALID UINT16_MAX
#define CONTROL_BATCH_SIZE 16

struct comb_bc_blob {
  void *data;
  size_t len;
};

struct guest_comb_bc {
  struct comb_bc_blob rx;
  struct comb_bc_blob deq;
  struct comb_bc_blob sched;
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
  /* Protocol registered for this guest */
  __u8 proto_type;

  /* Number of queues in shared memory */
  __u16 nqueues;
  /* List of queues for this protocol */
  struct proto_queue_control queues[MAX_PROTO_QUEUES];

  /* Number of maps created */
  __u16 nmaps;
};

#ifndef CHAM_CACHE_LINE_SIZE
#define CHAM_CACHE_LINE_SIZE 64
#endif

struct guest_budget_control {
  __s64 val;
  char __pad[CHAM_CACHE_LINE_SIZE - sizeof(__s64)];
} __attribute__((aligned(CHAM_CACHE_LINE_SIZE)));

struct guest_control {
  /* One budget counter per fast-path core shared with that core */
  struct guest_budget_control budgets[MAX_FP_CORES];

  /* ID of registered guest */
  __u8 id;

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
} __attribute__((aligned(CHAM_CACHE_LINE_SIZE)));

#if CHAM_CTL_BUDGET_STATS
struct ctl_budg_stats {
  __u64 nr;
  __u64 cyc;
};
#endif

struct control_context {
  /* Configuration parameters */
  struct configuration *config;
  /* Infra bytecode blob for combined JIT */
  struct comb_bc_blob comb_bc;
  /* Helper bytecode blob linked into aggregate combined JIT */
  struct comb_bc_blob comb_helpers_bc;
  /* Per-slot guest bytecode used to rebuild aggregate entries */
  struct guest_comb_bc guest_bc[CHAMELIO_MAX_GUESTS];
  /* Aggregate combined JIT objects */
  struct ebpf_vm_c *agg_rx_vm;
  struct ebpf_vm_c *agg_deq_vm;
  struct ebpf_vm_c *agg_sched_vm;
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
  /* Fast-path contexts owned by the top-level process */
  struct fast_context **f_ctxs;
  /* Last sampled fast-path batch counters. One per core */
  struct fast_stats *fast_batch_last;
  /* Last time fast-path batch stats were logged */
  __u64 fast_stats_tsc;
#if CHAM_CTL_BUDGET_STATS
  /* Cumulative budget refresh stats */
  struct ctl_budg_stats budg_stats;
  /* Last sampled budget refresh stats */
  struct ctl_budg_stats budg_last;
#endif
  
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
  struct guest_control guests[CHAMELIO_MAX_GUESTS];

/* Read-only after being initialized */
  /* Network virtualization table indexed by GRE key and inner IP */
  struct netvirt_table *inner_table;
  /* Network virtualization table indexed by guest ID and outer IP */
  struct netvirt_table *gid_table;
};

int control_context_init(struct control_context *ctl_ctx, 
    struct nic_context *nic_ctx, struct configuration *config, 
    struct shm_handle **fc_handles, struct shm_handle **cf_handles,
    struct shm_handle **txq_handles);
int control_loop(struct control_context *ctx);

#endif

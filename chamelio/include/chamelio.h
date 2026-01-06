#ifndef CHAMELIO_H_
#define CHAMELIO_H_

#include "config.h"
#include "nic.h"
#include "fast.h"
#include "control.h"
#include "shmalloc.h"
#include "netvirt.h"

struct chamelio_context {
  /* Configuration parameters */
  struct configuration config;
  /* Context for the NIC */
  struct nic_context nic_ctx;
  /* Context for the control-path  */
  struct control_context sp_ctx;
  /* Context for the fast-path in each core */
  struct fast_context **f_ctxs;
  /* Shared memory handles for the fast-path to control-path queues. 
     One per core. */
  struct shm_handle **fast_ctl_handles;
  /* Shared memory handles for the control-path to fast-path queues. 
     One per core. */
  struct shm_handle **ctl_fast_handles;
  /* Shared memory handles for the control-path to fast-path tx queues. 
     One per core */
  struct shm_handle **ctxq_handles;
  /* File descriptor for internal shared memory region */
  int shm_fd_internal;
  /* Base pointer to internal shared memory region */
  void *shm_base_internal;
  /* Read-only after being initialized */
  /* Network virtualization table indexed by GRE key and inner IP */
  struct netvirt_table inner_table;
  /* Network virtualization table indexed by guest ID and outer IP */
  struct netvirt_table gid_table;
};

#endif
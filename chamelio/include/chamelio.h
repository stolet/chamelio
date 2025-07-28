#ifndef CHAMELIO_H_
#define CHAMELIO_H_

#include "config.h"
#include "nic.h"
#include "fast.h"
#include "slow.h"
#include "shmalloc.h"

struct chamelio_context {
  /* Configuration parameters */
  struct configuration config;
  /* Context for the NIC */
  struct nic_context nic_ctx;
  /* Context for the slow-path  */
  struct slow_context sp_ctx;
  /* Context for the fast-path in each core */
  struct fast_context **f_ctxs;
  /* Shared memory handles for the fast-path to slow-path queues. One per core. */
  struct shm_handle **fast_slow_handles;
  /* Shared memory handles for the slow-path to fast-path queues. One per core. */
  struct shm_handle **slow_fast_handles;
};

#endif
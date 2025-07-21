#ifndef CHAMELIO_H_
#define CHAMELIO_H_

#include "config/config.h"
#include "nic/nic.h"
#include "fast/fast.h"
#include "slow/slow.h"

struct chamelio_context {
  /* Configuration parameters */
  struct configuration config;
  /* Context for the NIC */
  struct nic_context nic_ctx;
  /* Context for the slow-path  */
  struct slow_context sp_ctx;
  /* Context for the fast-path in each core */
  struct fast_context **f_ctxs;
  /* Queues from the fast-path to slow-path. One per core. */
  struct queue **fast_slow_qs;
  /* Queues from the slow-path to the fast-path. One per core */
  struct queue **slow_fast_qs;
};

#endif
#ifndef CHAMELEO_H_
#define CHAMELEO_H_

#include "config/config.h"
#include "nic/nic.h"
#include "fast/fast.h"
#include "slow/slow.h"

struct chameleo_context {
  struct configuration config;
  struct nic_context nic_ctx;
  struct slow_path_context sp_ctx;
  struct fast_path_context **fp_ctxs;
};

#endif
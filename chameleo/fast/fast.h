#ifndef FAST_H_
#define FAST_H_

#include <stdint.h>

#include <rte_ethdev.h>

#include "network.h" 
#include "../config/config.h"

#define BATCH_SIZE 16

struct fast_path_context {
  uint8_t id;
  struct network_context net_ctx;
};

int fast_path_context_init(struct fast_path_context *fp_ctx, 
    struct rte_eth_dev_info *eth_dev_info, 
    struct configuration *config, uint16_t thread_id, uint8_t port_id);
void fast_path_loop(struct fast_path_context *ctx);
void fast_path_context_destroy();

#endif
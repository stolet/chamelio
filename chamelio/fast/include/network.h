#ifndef NETWORK_H_
#define NETWORK_H_

#include <stdint.h>

#include <rte_mbuf.h>
#include <rte_ethdev.h>

#include "config.h"

struct network_context {
  uint8_t port_id;
  uint16_t queue_id;
  struct rte_mempool *pool;
};

int network_init(struct network_context *net_ctx, 
  struct rte_eth_dev_info *eth_dev_info, struct configuration *config,
  uint16_t queue_id,uint8_t port_id);
int network_rx(struct network_context *ctx, 
    unsigned num, struct rte_mbuf **mbs);
int network_tx(struct network_context *ctx, 
    unsigned num, struct rte_mbuf **mbs);

#endif
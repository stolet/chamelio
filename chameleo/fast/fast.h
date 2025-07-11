#ifndef FAST_H_
#define FAST_H_

#include <stdint.h>

#include <rte_ethdev.h>

#include "network.h" 
#include "../config/config.h"

#define BATCH_SIZE 16

/* We want the TXBUF_SIZE to be double the BATCH_SIZE so we can 
   fit packets from the TX phase and ACKs sent in the receive phase */
#define TXBUF_SIZE 2 * BATCH_SIZE

#define PROTOCOL_UDP 1
#define PROTOCOL_TCP 2
#define PROTOCOL_RDMA 3

struct protocol {
  uint8_t id;

  int (*process_rx)(struct rte_mbuf *);
  int (*process_tx)(struct rte_mbuf *);
  int (*process_queues)();
};

struct application {
  uint8_t id;
  struct protocol proto;
};

struct guest {
  uint8_t id;
  struct guest *next_guest;

  uint8_t n_apps;
  struct application *apps;
};

struct fast_path_context {
  uint8_t id;
  struct network_context net_ctx;
  
  uint8_t n_guests;
  struct guest *guests;

  uint16_t tx_n;
  struct rte_mbuf *tx_mbs[TXBUF_SIZE];
};

int fast_path_context_init(struct fast_path_context *fp_ctx, 
    struct rte_eth_dev_info *eth_dev_info, 
    struct configuration *config, uint16_t thread_id, uint8_t port_id);
int fast_path_loop(struct fast_path_context *ctx);
void fast_path_context_destroy();

#endif
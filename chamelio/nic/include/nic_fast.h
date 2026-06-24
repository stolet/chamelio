#ifndef NIC_FAST_H_
#define NIC_FAST_H_

#include <linux/types.h>

#include <rte_mbuf.h>
#include <rte_ethdev.h>

#include "config.h"
#include "nic.h"

/* NIC context in the fast-path */
struct nic_fast_context {
  /* Port ID of the initialised NIC */
  __u8 port_id;
  /* NIC queue ID for this fast-path core */
  __u16 queue_id;
  /* Memory pool for this fast-path core */
  struct rte_mempool *pool;
  /* Ethernet address of the NIC port */
  struct rte_ether_addr eth_addr;
  /* NIC supports GRE tunnel TX checksum offload */
  __u8 hw_gre_csum;
};

/* Initialises NIC queue and memory pool for this core */
int nic_fast_init(struct nic_context *nic_ctx,
    struct nic_fast_context *nic_fast_ctx, __u16 queue_id,
    struct configuration *config);

/* Dequeues mbufs from the NIC queue */
static inline int nic_fast_rx(struct nic_fast_context *ctx, 
    unsigned num, struct rte_mbuf **mbs)
{
  __u8 port_id;
  __u16 queue_id;
  
  port_id = ctx->port_id;
  queue_id = ctx->queue_id;
  num = rte_eth_rx_burst(port_id, queue_id, mbs, num);

  return num;
}

/* Enqueues mbufs for transmission in NIC queue */
static inline int nic_fast_tx(struct nic_fast_context *ctx, 
    unsigned num, struct rte_mbuf **mbs)
{
  unsigned sent;
  __u8 port_id;
  __u16 queue_id;

  if (num == 0)
    return 0;
  
  port_id = ctx->port_id;
  queue_id = ctx->queue_id;

  sent = rte_eth_tx_burst(port_id, queue_id, mbs, num);

  return sent;
}

#endif

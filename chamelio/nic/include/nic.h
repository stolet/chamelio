#ifndef NIC_H_
#define NIC_H_

#include <stdint.h>

#include <rte_ethdev.h>

#include "config.h"

/* NIC context used by Chamelio main theread */
struct nic_context {
  /* Port ID of initialised NIC */
  uint8_t port_id;
  /* DPDK port configuration */
  struct rte_eth_conf port_conf;
  /* DPDK device information of initialised NIC */
  struct rte_eth_dev_info eth_dev_info;
  /* Ethernet address of the NIC port */
  struct rte_ether_addr eth_addr;
};

/* Initialises a NIC port */
int nic_init(struct nic_context *nic_ctx, struct configuration *config);
/* Cleanup the NIC port */
void nic_cleanup(struct nic_context *nic_ctx);

#endif
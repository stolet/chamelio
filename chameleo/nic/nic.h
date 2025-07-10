#ifndef NIC_H_
#define NIC_H_

#include <stdint.h>

#include <rte_ethdev.h>

#include "../chameleo.h"

struct nic_context {
  uint8_t port_id;
  struct rte_eth_conf port_conf;
  struct rte_eth_dev_info eth_dev_info;
  struct rte_ether_addr eth_addr;
};

int nic_init();
void nic_cleanup(struct nic_context *nic_ctx);
int nic_fp_init();

#endif
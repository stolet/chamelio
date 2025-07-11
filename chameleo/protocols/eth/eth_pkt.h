#ifndef ETH_PKT_H_
#define ETH_PKT_H_

#include <stdint.h>

#include "../../../utils/utils.h"

#define ETH_ADDR_LEN 6

#define ETH_TYPE_IP   0x0800
#define ETH_TYPE_ARP  0x0806

struct cham_eth_addr {
  uint8_t addr[ETH_ADDR_LEN];
} __attribute__ ((packed));

struct eth_hdr {
  struct cham_eth_addr dst;
  struct cham_eth_addr src;
  beui16_t type;
} __attribute__ ((packed));

struct eth_pkt {
  struct eth_hdr eth;
} __attribute__ ((packed));

#endif
#ifndef ETH_HDR_H_
#define ETH_HDR_H_

#include "utils.h"

#define ETH_TYPE_IP   0x0800
#define ETH_TYPE_ARP  0x0806
#define ETH_ADDR_LEN 6

struct eth_addr {
  __u8 addr[ETH_ADDR_LEN];
} __attribute__ ((packed));

struct eth_hdr {
  /* Destination MAC address */
  struct eth_addr dst;
  /* Source MAC address */
  struct eth_addr src;
  /* Type of next header */
  beui16_t type;
} __attribute__ ((packed));

#endif
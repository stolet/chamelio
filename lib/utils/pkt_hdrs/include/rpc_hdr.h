#ifndef RPC_HDR_H_
#define RPC_HDR_H_

#include "utils.h"
#include "eth_hdr.h"
#include "ip_hdr.h"
#include "udp_hdr.h"

struct rpc_hdr {
  /* Service type */
  beui16_t service;
  /* Length including this header and data */
  beui16_t len;
  /* Unique request identifier */
  beui32_t rid;
  /* Type field: 0 for request, 1 for reply */
  __u8 type;
} __attribute__ ((packed));

struct rpc_pkt {
  struct eth_hdr eth;
  struct ip_hdr ip;
  struct udp_hdr udp;
  struct rpc_hdr rpc;
} __attribute__ ((packed));

#endif
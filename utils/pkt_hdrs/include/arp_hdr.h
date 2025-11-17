#ifndef ARP_HDR_H_
#define ARP_HDR_H_

#include "utils.h"
#include "eth_hdr.h"
#include "ip_hdr.h"

#define ARP_OPER_REQUEST 1
#define ARP_OPER_REPLY 2
#define ARP_HTYPE_ETHERNET 1
#define ARP_PTYPE_IPV4 0x0800

struct arp_hdr {
  beui16_t htype;
  beui16_t ptype;
  __u8 hlen;
  __u8 plen;
  beui16_t oper;
  struct eth_addr sha;
  ip_addr_t spa;
  struct eth_addr tha;
  ip_addr_t tpa;
} __attribute__((packed));

#endif
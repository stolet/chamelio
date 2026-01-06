#ifndef ARP_HDR_H_
#define ARP_HDR_H_

#include "utils.h"
#include "eth_hdr.h"
#include "ip_hdr.h"

#define ARP_OPER_REQUEST 1
#define ARP_OPER_REPLY 2
#define ARP_HTYPE_ETHERNET 1
#define ARP_PTYPE_IPV4 0x0800

/* ARP Header */
struct arp_hdr {
  /* Hardware type */
  beui16_t htype;
  /* Protocol type */
  beui16_t ptype;
  /* Hardware length */
  __u8 hlen;
  /* Protocol length */
  __u8 plen;
  /* Operation */
  beui16_t oper;
  /* Sender hardware address */
  struct eth_addr sha;
  /* Sender protocol address */
  ip_addr_t spa;
  /* Target hardware address */
  struct eth_addr tha;
  /* Target protocol address */
  ip_addr_t tpa;
} __attribute__((packed));

struct arp_pkt {
  struct eth_hdr eth;
  struct arp_hdr arp;
} __attribute__ ((packed));

#endif
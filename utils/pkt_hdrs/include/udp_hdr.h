#ifndef UDP_HDR_H_
#define UDP_HDR_H_

#include "utils.h"
#include "eth_hdr.h"
#include "ip_hdr.h"

struct udp_hdr {
  /* Src port */
  beui16_t src;
  /* Destination port */
  beui16_t dst;
  /* Length of header and data */
  beui16_t len;
  /* Checksum */
  __u16 chksum;
} __attribute__ ((packed));

struct udp_pkt {
  struct eth_hdr eth;
  struct ip_hdr ip;
  struct udp_hdr udp;
} __attribute__ ((packed));

#endif
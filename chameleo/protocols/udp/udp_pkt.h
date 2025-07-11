#ifndef UDP_PKT_H_
#define UDP_PKT_H_

#include <stdint.h>

#include "../eth/eth_pkt.h"
#include "../ip/ip_pkt.h"
#include "../../../utils/utils.h"

struct udp_hdr {
  /* src port */
  beui16_t src;
  /* destination port */
  beui16_t dst;
  /* length of header and data */
  beui16_t len;
  /* checksum */
  uint16_t chksum;
} __attribute__ ((packed));

struct udp_pkt {
  struct eth_hdr eth;
  struct ip_hdr ip;
  struct udp_hdr udp;
} __attribute__ ((packed));

#endif
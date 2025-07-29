#ifndef PKT_DEFS_H_
#define PKT_DEFS_H_

#include <stdint.h>

#include "utils.h"

/* ETH */

#define ETH_ADDR_LEN 6

#define ETH_TYPE_IP   0x0800
#define ETH_TYPE_ARP  0x0806

struct eth_addr {
  uint8_t addr[ETH_ADDR_LEN];
} __attribute__ ((packed));

struct eth_hdr {
  struct eth_addr dst;
  struct eth_addr src;
  beui16_t type;
} __attribute__ ((packed));

struct eth_pkt {
  struct eth_hdr eth;
} __attribute__ ((packed));

/* IP */

#define IPH_V(hdr)  ((hdr)->_v_hl >> 4)
#define IPH_HL(hdr) ((hdr)->_v_hl & 0x0f)
#define IPH_TOS(hdr) ((hdr)->_tos)
#define IPH_ECN(hdr) ((hdr)->_tos & 0x3)

#define IPH_VHL_SET(hdr, v, hl) (hdr)->_v_hl = (((v) << 4) | (hl))
#define IPH_TOS_SET(hdr, tos) (hdr)->_tos = (tos)
#define IPH_ECN_SET(hdr, e) (hdr)->_tos = ((hdr)->_tos & 0xffc) | (e)

#define IP_HLEN 20

#define IP_PROTO_IP      0
#define IP_PROTO_ICMP    1
#define IP_PROTO_IGMP    2
#define IP_PROTO_IPENCAP 4
#define IP_PROTO_UDP     17
#define IP_PROTO_UDPLITE 136
#define IP_PROTO_TCP     6
#define IP_PROTO_DCCP	   33
#define IP_PROTO_GRE     47

#define CHAM_IP_ECN_NONE      0x0
#define CHAM_IP_ECN_ECT0      0x2
#define CHAM_IP_ECN_ECT1      0x1
#define CHAM_IP_ECN_CE        0x3

typedef beui32_t ip_addr_t;

struct ip_hdr {
  /* version / header length */
  uint8_t _v_hl;
  /* type of service */
  uint8_t _tos;
  /* total length */
  beui16_t len;
  /* identification */
  beui16_t id;
  /* fragment offset field */
  beui16_t offset;
  /* time to live */
  uint8_t ttl;
  /* protocol*/
  uint8_t proto;
  /* checksum */
  uint16_t chksum;
  /* source IP address */
  ip_addr_t src;
  /* destination IP address */
  ip_addr_t dst;
} __attribute__ ((packed));

struct ip_pkt {
  struct eth_hdr eth;
  struct ip_hdr  ip;
} __attribute__ ((packed));


/* UDP */

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
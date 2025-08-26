#ifndef UDP_FAST_H_
#define UDP_FAST_H_

#include "utils.h"

/*** ETH ***/

#define ETH_TYPE_IP   0x0800
#define ETH_TYPE_ARP  0x0806
#define ETH_ADDR_LEN 6

struct eth_addr {
  uint8_t addr[ETH_ADDR_LEN];
} __attribute__ ((packed));

struct eth_hdr {
  /* Destination MAC address */
  struct eth_addr dst;
  /* Source MAC address */
  struct eth_addr src;
  beui16_t type;
} __attribute__ ((packed));

/*** IP ***/

/* Gets version from IP header */
#define IPH_V(hdr)  ((hdr)->_v_hl >> 4)
/* Gets header length from IP header */
#define IPH_HL(hdr) ((hdr)->_v_hl & 0x0f)
/* Gets the type of service from the IP header */
#define IPH_TOS(hdr) ((hdr)->_tos)
/* Gets ECN value from the IP header */
#define IPH_ECN(hdr) ((hdr)->_tos & 0x3)

/* Sets version and header length */
#define IPH_VHL_SET(hdr, v, hl) (hdr)->_v_hl = (((v) << 4) | (hl))
/* Sets the type of service */
#define IPH_TOS_SET(hdr, tos) (hdr)->_tos = (tos)
/* Sets ECN value */
#define IPH_ECN_SET(hdr, e) (hdr)->_tos = ((hdr)->_tos & 0xffc) | (e)

#define IP_HLEN 20

/* Values for IP protocol types */
#define IP_PROTO_IP      0
#define IP_PROTO_ICMP    1
#define IP_PROTO_IGMP    2
#define IP_PROTO_IPENCAP 4
#define IP_PROTO_UDP     17
#define IP_PROTO_UDPLITE 136
#define IP_PROTO_TCP     6
#define IP_PROTO_DCCP	   33
#define IP_PROTO_GRE     47

/* No congestion experienced */
#define CHAM_IP_ECN_NONE      0x0
/* ECN capable transport */
#define CHAM_IP_ECN_ECT0      0x2
/* ECN capable transport */
#define CHAM_IP_ECN_ECT1      0x1
/* Congestion experienced */
#define CHAM_IP_ECN_CE        0x3

typedef beui32_t ip_addr_t;

struct ip_hdr {
  /* Version / header length */
  uint8_t _v_hl;
  /* Type of service */
  uint8_t _tos;
  /* Total length */
  beui16_t len;
  /* Identification */
  beui16_t id;
  /* Fragment offset field */
  beui16_t offset;
  /* Time to live */
  uint8_t ttl;
  /* Protocol*/
  uint8_t proto;
  /* Checksum */
  uint16_t chksum;
  /* Source IP address */
  ip_addr_t src;
  /* Destination IP address */
  ip_addr_t dst;
} __attribute__ ((packed));

/*** UDP ***/

struct udp_hdr {
  /* Src port */
  beui16_t src;
  /* Destination port */
  beui16_t dst;
  /* Length of header and data */
  beui16_t len;
  /* Checksum */
  uint16_t chksum;
} __attribute__ ((packed));

struct udp_pkt {
  struct eth_hdr eth;
  struct ip_hdr ip;
  struct udp_hdr udp;
} __attribute__ ((packed));

int udp_event_rx(void *pkt, void *shm, void *map);
int udp_event_tx(void *pkt, void *shm, void *map);
int udp_event_deq(int qid, void *shm, void *map);
int udp_act_txsched(int n, void *shm, void *map);

#endif
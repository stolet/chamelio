#ifndef UDP_FAST_H_
#define UDP_FAST_H_

#include "utils.h"

/*** ETH ***/

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
int udp_event_deq(int qid, void *qe);

int udp_act_enq(int qid, void *qe);
int udp_act_txsched(int n, void *shm, void *map);

#endif
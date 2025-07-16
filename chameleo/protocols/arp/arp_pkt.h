// #ifndef ARP_PKT_H_
// #define ARP_PKT_H_

// #include <stdint.h>
// #include "../../../utils/utils.h"
// #include "../ip/ip_pkt.h"
// #include "../eth/eth_pkt.h"

// #define ARP_OPER_REQUEST 1
// #define ARP_OPER_REPLY 2
// #define ARP_HTYPE_ETHERNET 1
// #define ARP_PTYPE_IPV4 0x0800

// struct arp_hdr {
//   beui16_t htype;
//   beui16_t ptype;
//   uint8_t hlen;
//   uint8_t plen;
//   beui16_t oper;
//   struct eth_addr sha;
//   ip_addr_t spa;
//   struct eth_addr tha;
//   ip_addr_t tpa;
// } __attribute__((packed));

// #endif
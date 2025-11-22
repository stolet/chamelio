#ifndef LOG_PKT_H_
#define LOG_PKT_H_

#include "eth_hdr.h"
#include "ip_hdr.h"
#include "udp_hdr.h"
#include "arp_hdr.h"

void log_eth(const struct eth_hdr *eth);
void log_ip(const struct ip_hdr *ip);
void log_udp(const struct udp_hdr *udp);
void log_arp(const struct arp_hdr *arp);
void log_arp_pkt(const struct pkt_arp *p);
void log_udp_pkt(const struct udp_pkt *p);

#endif
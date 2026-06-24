#ifndef LOG_PKT_H_
#define LOG_PKT_H_

#include "eth_hdr.h"
#include "ip_hdr.h"
#include "tcp_hdr.h"
#include "udp_hdr.h"
#include "arp_hdr.h"
#include "log.h"

void log_eth(const struct eth_hdr *eth);
void log_ip(const struct ip_hdr *ip);
void log_tcp(const struct tcp_hdr *tcp);
void log_udp(const struct udp_hdr *udp);
void log_arp(const struct arp_hdr *arp);
void log_arp_pkt(const struct arp_pkt *p);
void log_tcp_pkt(const struct tcp_pkt *p);
void log_udp_pkt(const struct udp_pkt *p);
void log_udp_pkt_inner(const struct udp_pkt_inner *p);

/* Call a log_pkt function only when debug logging is enabled. */
#define LOG_PKT_DEBUG(fn, ...) do { \
  if (cham_log_level >= CHAM_LOG_LEVEL_DEBUG) \
    fn(__VA_ARGS__); \
} while (0)

#endif
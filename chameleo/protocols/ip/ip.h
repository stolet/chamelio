#ifndef IP_H_
#define IP_H_

#include <rte_mbuf.h>
#include <arpa/inet.h>

#include "ip_pkt.h"
#include "../udp/udp_pkt.h"
#include "../../../utils/log/log.h"
#include "../../../utils/utils.h"

static inline int ip_process_rx(struct rte_mbuf *mb)
{
  struct ip_pkt *p = (struct ip_pkt *) (mb->buf_addr + mb->data_off);

  LOG_DEBUG("rx ip: dst_ip=%08x src_ip=%08x", 
      f_beui32(p->ip.dst), f_beui32(p->ip.src));
  return 0;
}

static inline int ip_process_tx(struct rte_mbuf *mb)
{
  uint16_t opt_len, hdrs_len, payload_len;
  struct ip_pkt *p = (struct ip_pkt *) (mb->buf_addr + mb->data_off);

  opt_len = 0;
  payload_len = 0;
  /* TODO: Pass protocol type we are looking at here instead of hardcoding udp_hdr */
  hdrs_len = sizeof(struct ip_hdr) + sizeof(struct udp_hdr) + opt_len;

  struct in_addr src, dst;
  uint32_t src_be, dst_be;
  inet_pton(AF_INET, "192.168.10.14", &src);
  inet_pton(AF_INET, "192.168.10.13", &dst);
  src_be = (uint32_t) src.s_addr;
  dst_be = (uint32_t ) dst.s_addr;

  IPH_VHL_SET(&p->ip, 4, 5);
  p->ip._tos = 0;
  p->ip.len = t_beui16(hdrs_len + payload_len);
  p->ip.id = t_beui16(3); /* not sure why we have 3 here */
  p->ip.offset = t_beui16(0);
  p->ip.ttl = 0xff;
  p->ip.proto = IP_PROTO_UDP;
  p->ip.chksum = 0;
  p->ip.src = t_beui32(ntohl(src_be));
  p->ip.dst = t_beui32(ntohl(dst_be));

  p->ip.chksum = rte_ipv4_cksum((void *) &p->ip);

  LOG_DEBUG("tx ip: dst_ip=%08x src_ip=%08x", 
      f_beui32(p->ip.dst), f_beui32(p->ip.src));

  return 0;
}

static inline int ip_process_queues()
{
  return 0;
}

#endif
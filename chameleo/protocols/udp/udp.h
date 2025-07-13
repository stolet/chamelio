#ifndef UDP_H_
#define UDP_H_

#include <rte_mbuf.h>

#include "udp_pkt.h"
#include "../../../utils/log/log.h"
#include "../../../utils/utils.h"

static inline int udp_process_rx(struct rte_mbuf *mb)
{
  struct udp_pkt *p = (struct udp_pkt *) (mb->buf_addr + mb->data_off);

  LOG_DEBUG("rx udp: src_port=%d dst_port=%d", 
      f_beui16(p->udp.src), f_beui16(p->udp.dst));

  return 0;
}

static inline int udp_process_tx(struct rte_mbuf *mb)
{
  uint16_t opt_len, hdrs_len, payload_len;
  struct udp_pkt *p = (struct udp_pkt *) (mb->buf_addr + mb->data_off);
  
  /* TODO: Calculate payload and opt len */
  opt_len = 0;
  payload_len = 0;
  hdrs_len = sizeof(struct udp_hdr) + opt_len;

  /* Checksum has to be 0 before we can compute it */
  p->udp.chksum = 0;

  p->udp.src = t_beui16(1234);
  p->udp.dst = t_beui16(1235);
  p->udp.chksum = rte_ipv4_udptcp_cksum((void *) &p->ip, (void *) &p->udp);
  p->udp.len = t_beui16(hdrs_len + payload_len);

  LOG_DEBUG("tx udp: src_port=%d dst_port=%d", 
      f_beui16(p->udp.src), f_beui16(p->udp.dst));

  return 0;
}

static inline int udp_process_queues()
{
  return 0;
}

static inline int udp_pkt_len()
{
  uint16_t opt_len, hdrs_len, payload_len;

  /* TODO: Calculate payload and opt len */
  opt_len = 0;
  payload_len = 0;
  hdrs_len = sizeof(struct udp_pkt) + opt_len;

  return hdrs_len + payload_len;
}

#endif
#include <rte_ip4.h>

#include "udp.h"
#include "udp_fast.h"
#include "udp_queue.h"
#include "log.h"

int mac_from_text(const char *text, uint8_t out[ETH_ADDR_LEN])
{
    unsigned int tmp[ETH_ADDR_LEN];

    if (sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x", 
        &tmp[0], &tmp[1], &tmp[2], &tmp[3], &tmp[4], &tmp[5]) != 6)
        return -1;          

    for (size_t i = 0; i < ETH_ADDR_LEN; ++i)
        out[i] = (uint8_t)tmp[i];

    return 0;             
}

int udp_event_rx(void *pkt, void *shm, void *map)
{
  struct udp_pkt *p = (struct udp_pkt *) pkt;

  LOG_DEBUG("rx udp: src_port=%d dst_port=%d", 
      f_beui16(p->udp.src), f_beui16(p->udp.dst));

  return 0;
}

int udp_event_tx(void *pkt, void *shm, void *map)
{
  int ret;
  uint16_t opt_len, payload_len;
  uint16_t udp_hdrs_len, ip_hdrs_len, pkt_hdrs_len;
  uint64_t mac_src_val, mac_dst_val;
  struct eth_addr mac_src, mac_dst;
  struct in_addr ip_src, ip_dst;
  uint32_t ip_src_be, ip_dst_be;
  struct udp_txready_mape *ready_entry;
  struct udp_sock_mape *sock_entry;
  struct udp_bump_mape *bump_entry;
  struct udp_queue_entry *qe;
  struct udp_queue_bump *bump; 

  struct udp_off_mape *off_table = map;
  struct udp_sock_mape *sock_table = shm + off_table[MTYPE_SOCKS].off;
  struct udp_txready_mape *ready_table = shm + off_table[MTYPE_TXREADY].off;
  struct udp_bump_mape *bump_table = shm + off_table[MTYPE_BUMPQ].off;
  struct udp_pkt *p = (struct udp_pkt *) pkt;

  /* Get first entry in ready table */
  ready_entry = &ready_table[off_table[MTYPE_TXREADY].head];
  sock_entry = &sock_table[ready_entry->sock_id];
  
  /* TODO: Calculate payload and opt len */
  opt_len = 0;
  payload_len = ready_entry->tx_ready;
  udp_hdrs_len = sizeof(struct udp_hdr) + opt_len;
  ip_hdrs_len = sizeof(struct ip_hdr) + sizeof(struct udp_hdr) + opt_len;
  pkt_hdrs_len = sizeof(struct eth_hdr) + sizeof(struct ip_hdr) 
    + sizeof(struct udp_hdr) + opt_len;

  /* Set ETH header */
  mac_from_text("b8:59:9f:c4:af:66", mac_src.addr);
  mac_from_text("b8:59:9f:c4:af:e6", mac_dst.addr);

  p->eth.src = mac_src;
  p->eth.dst = mac_dst;
  p->eth.type = t_beui16(ETH_TYPE_IP);
  memcpy(&mac_src_val, &p->eth.src, ETH_ADDR_LEN);
  memcpy(&mac_dst_val, &p->eth.dst, ETH_ADDR_LEN);
  LOG_DEBUG("tx eth: src_mac=%012" PRIx64 " dst_mac=%012" PRIx64,
          (uint64_t)(be64toh(mac_src_val)),
          (uint64_t)(be64toh(mac_dst_val)));

  /* Set IP header */
  inet_pton(AF_INET, "192.168.10.14", &ip_src);
  inet_pton(AF_INET, "192.168.10.13", &ip_dst);
  ip_src_be = (uint32_t) ip_src.s_addr;
  ip_dst_be = (uint32_t ) ip_dst.s_addr;

  IPH_VHL_SET(&p->ip, 4, 5);
  p->ip._tos = 0;
  p->ip.len = t_beui16(ip_hdrs_len + payload_len);
  p->ip.id = t_beui16(3); /* not sure why we have 3 here */
  p->ip.offset = t_beui16(0);
  p->ip.ttl = 0xff;
  p->ip.proto = IP_PROTO_UDP;
  p->ip.chksum = 0;
  p->ip.src = t_beui32(ntohl(ip_src_be));
  p->ip.dst = t_beui32(ntohl(ip_dst_be));

  p->ip.chksum = rte_ipv4_cksum((void *) &p->ip);

  /* Set UDP header */
  /* TODO: Add actual port */
  p->udp.src = t_beui16(1234);
  p->udp.dst = t_beui16(1235);
  
  /* Checksum has to be 0 before we can compute it */
  p->udp.chksum = 0;
  p->udp.chksum = rte_ipv4_udptcp_cksum((void *) &p->ip, (void *) &p->udp);
  p->udp.len = t_beui16(udp_hdrs_len + payload_len);

  /* Copy data to packet */
  memcpy(p + pkt_hdrs_len, 
      sock_entry->tx_buf + sock_entry->tx_head, payload_len);
  sock_entry->tx_head += payload_len;
  sock_entry->tx_avail -= payload_len;

  /* Update head of ready queue */
  off_table[MTYPE_TXREADY].head = ready_entry->next_id;
  ready_entry->next_id = ID_INVALID;

  /* Send a bump to application */
  bump_entry = &bump_table[sock_entry->app_bump_qid];
  qe = udp_queue_tail(&bump_entry->q);
  if (qe == NULL)
  {
    LOG_DEBUG("failed to get bump queue tail");
    return -1;
  }
  bump = &qe->data.bump;
  
  bump->rx_avail = 0;
  bump->rx_head = 0;
  bump->tx_avail = 0;
  bump->tx_head = payload_len;
  ret = udp_queue_enqueue(&bump_entry->q, UDP_QUEUE_BUMP);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue bump message");
    return -1;
  }

  LOG_DEBUG("tx udp: src_port=%d dst_port=%d", 
      f_beui16(p->udp.src), f_beui16(p->udp.dst));
  LOG_DEBUG("tx ip: dst_ip=%08x src_ip=%08x", 
      f_beui32(p->ip.dst), f_beui32(p->ip.src));
  LOG_DEBUG("tx eth: src_mac=%012" PRIx64 " dst_mac=%012" PRIx64,
          (uint64_t)(be64toh(mac_src_val)),
          (uint64_t)(be64toh(mac_dst_val)));

  return 0;
}

int udp_act_txsched(int n, void *shm, void *map)
{
  int n_sched;
  uint32_t id, bytes;
  struct udp_txsched_mape *cur_sched;
  struct udp_txready_mape *cur_ready;

  struct udp_off_mape *off_table = map;
  struct udp_txsched_mape *sched_table = shm + off_table[MTYPE_TXSCHED].off;
  struct udp_txready_mape *ready_table = shm + off_table[MTYPE_TXREADY].off;
  
  for (n_sched = 0; n_sched < n && off_table[MTYPE_TXSCHED].head != ID_INVALID;)
  {
    id = off_table[MTYPE_TXSCHED].head;
    cur_sched = &sched_table[id];

    /* If data is available schedule this socket for transmission */
    if (cur_sched->tx_avail > 0)
    {
      bytes = cur_sched->tx_avail;
      if (cur_sched->tx_avail > UDP_MSS)
        bytes = UDP_MSS;

      /* Add to ready table */
      cur_ready = &ready_table[n_sched];
      cur_ready->id = n_sched;
      cur_ready->sock_id = cur_sched->id;
      cur_ready->tx_ready = bytes;
      cur_ready->next_id = ID_INVALID;

      /* Table is empty */
      if (off_table[MTYPE_TXREADY].tail == ID_INVALID)
        off_table[MTYPE_TXREADY].head = cur_ready->id;
      else 
        ready_table[off_table[MTYPE_TXREADY].tail].next_id = cur_ready->id;

      off_table[MTYPE_TXREADY].tail = cur_ready->id;

      cur_sched->tx_avail -= bytes;
      n_sched++;
    }
    
    /* If entry still has data available add to the back of the queue */
    if (cur_sched->tx_avail > 0)
    {

      /* Table is empty */
      if (off_table[MTYPE_TXSCHED].tail == ID_INVALID)
        off_table[MTYPE_TXSCHED].head = cur_sched->id;
      else
        sched_table[off_table[MTYPE_TXSCHED].tail].next_id = cur_sched->id;

      off_table[MTYPE_TXSCHED].tail = cur_sched->id;
    }

    off_table[MTYPE_TXSCHED].head = cur_sched->next_id;
    cur_sched->next_id = ID_INVALID;
  }

  return n_sched;
}

int udp_event_deq(int qid, void *qe)
{
  return 0;
}

int udp_act_enq(int qid, void *qe)
{
  return 0;
}
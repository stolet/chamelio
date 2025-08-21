#include <rte_ip4.h>

#include "udp.h"
#include "udp_fast.h"
#include "log.h"

int udp_event_rx(void *pkt, void *shm, void *map)
{
  struct udp_pkt *p = (struct udp_pkt *) pkt;

  LOG_DEBUG("rx udp: src_port=%d dst_port=%d", 
      f_beui16(p->udp.src), f_beui16(p->udp.dst));

  return 0;
}

int udp_event_tx(void *pkt, void *shm, void *map)
{
  uint16_t opt_len, hdrs_len, payload_len;
  struct udp_socket_slow *sock;

  struct udp_off_mape *off_table = map;
  struct udp_sock_mape *sock_table = shm + off_table[MTYPE_SOCKS].off;
  struct udp_pkt *p = (struct udp_pkt *) pkt;
  
  /* TODO: Calculate payload and opt len */
  opt_len = 0;
  payload_len = 0;
  hdrs_len = sizeof(struct udp_hdr) + opt_len;

  /* Set IP address and port */

  /* Checksum has to be 0 before we can compute it */
  p->udp.chksum = 0;
  /* TODO: Add actual port */
  p->udp.src = t_beui16(1234);
  p->udp.dst = t_beui16(1235);
  p->udp.chksum = rte_ipv4_udptcp_cksum((void *) &p->ip, (void *) &p->udp);
  p->udp.len = t_beui16(hdrs_len + payload_len);

  LOG_DEBUG("tx udp: src_port=%d dst_port=%d", 
      f_beui16(p->udp.src), f_beui16(p->udp.dst));

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
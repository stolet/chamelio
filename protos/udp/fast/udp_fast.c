#include <rte_ip4.h>
#include <cham_fast.h>
#include <cham_scheduler.h>

#include "udp.h"
#include "udp_fast.h"
#include "udp_queue.h"
#include "queue.h"
#include "log.h"

#define SOCK_MAP_IDX 0

int mac_from_text(const char *text, uint8_t out[ETH_ADDR_LEN]);

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

int udp_init_fp(void *config)
{
  return 0;
}

int udp_event_rx(void *pkt)
{
  struct udp_pkt *p = (struct udp_pkt *)pkt;

  LOG_DEBUG("rx udp: src_port=%d dst_port=%d",
            f_beui16(p->udp.src), f_beui16(p->udp.dst));

  return 0;
}

int udp_event_tx(void *pkt, struct cham_proto_handle *handle)
{
  int ret;
  void *payload;
  struct udp_sock *sock;
  struct equeue *q;
  struct udp_queue_entry *qe;
  struct udp_queue_bump *bump;
  struct cham_scheduler *sched;
  struct cham_sched_entry *sched_entry;
  uint16_t opt_len, payload_len;
  uint16_t udp_hdrs_len, ip_hdrs_len, pkt_hdrs_len;
  uint64_t mac_src_val, mac_dst_val, part;
  struct eth_addr mac_src, mac_dst;
  struct in_addr ip_src, ip_dst;
  uint32_t ip_src_be, ip_dst_be;
  struct udp_pkt *p = (struct udp_pkt *) pkt;
  
  /* If there is nothing scheduled return */
  sched = &handle->sched;
  sched_entry = sched_head(sched);
  if (sched_entry == NULL)
    return -1;

  /* Calculate number of bytes to transmit */
  payload_len = sched_entry->avail;
  if (payload_len > UDP_MSS)
    payload_len = UDP_MSS;

    
  /* TODO: Calculate payload and opt len */
  opt_len = 0;
  udp_hdrs_len = sizeof(struct udp_hdr) + opt_len;
  ip_hdrs_len = sizeof(struct ip_hdr) + sizeof(struct udp_hdr) + opt_len;
  pkt_hdrs_len = sizeof(struct eth_hdr) + sizeof(struct ip_hdr)
    + sizeof(struct udp_hdr) + opt_len;

  sock = (struct udp_sock *) sched_entry->opaque;
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
  ip_dst_be = sock->dst_ip;
  /* TODO: set actual IP*/
  ip_src_be = (uint32_t) ip_src.s_addr;

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
  p->udp.dst = t_beui16(sock->dst_port);
  /* TODO: Add actual port */
  p->udp.src = t_beui16(1234);

  /* Checksum has to be 0 before we can compute it */
  p->udp.chksum = 0;
  p->udp.chksum = rte_ipv4_udptcp_cksum((void *) &p->ip, (void *) &p->udp);
  p->udp.len = t_beui16(udp_hdrs_len + payload_len);

  /* Copy data to packet */
  payload = pkt + pkt_hdrs_len;
  uint8_t bleh[payload_len];
  if (sock->tx_head + payload_len <= sock->tx_len) 
  {
    // memcpy(payload, sock->tx_buf + sock->tx_head, payload_len);
    memcpy(payload, bleh, payload_len);
  } 
  else 
  {
    part = sock->tx_len - (sock->tx_head + payload_len);
    memcpy(payload, handle->shm_base + sock->tx_off + sock->tx_head, part);
    memcpy(payload + part, handle->shm_base + sock->tx_off, payload_len - part);
  }
  sock->tx_head += payload_len;
  sock->tx_avail -= payload_len;
  sched_entry->avail -= payload_len;

  /* Remove first element from priority list */
  ret = sched_pop(sched);
  if (ret != 0)
  {
    LOG_ERROR("failed to pop entry from scheduler");
    return -1;
  }
  
  /* Add entry to the back if there is still data to send */
  if (sched_entry->avail > 0)
  {
    ret = sched_add(sched, sock->id, 0, 
        sched_entry->avail, sched_entry->opaque);
    if (ret != 0)
    {
      LOG_ERROR("failed to re-add entry to scheduler");
      return -1;
    }
  }

  /* Send a bump to application */
  q = &handle->equeues[sock->app_bump_qid].eq;
  qe = queue_tail(q);
  if (qe == NULL)
  {
    LOG_ERROR("failed to get bump queue tail");
    return -1;
  }

  bump = &qe->data.bump;
  bump->opaque = sock->opaque;
  bump->rx_avail = 0;
  bump->rx_head = 0;
  bump->tx_avail = 0;
  bump->tx_head = payload_len;

  ret = queue_enqueue(q, UDP_QUEUE_BUMP);
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

int udp_event_deq(int qid, struct queue_entry *qe,
  struct cham_proto_handle *handle)
{
  int ret;
  struct udp_sock *sock;
  struct udp_sock *sock_map;
  struct cham_scheduler *sched;
  struct cham_sched_entry *se;
  struct udp_queue_entry *udp_qe;
  struct udp_queue_bump *bump;
  
  sock_map = handle->maps[SOCK_MAP_IDX].addr;
  udp_qe = (struct udp_queue_entry *) qe;
  bump = &udp_qe->data.bump;
  sock = &sock_map[bump->sock_id];
  sock->tx_avail += bump->tx_avail;
  sock->rx_head += bump->rx_head;

  if (bump->tx_avail > 0)
  {
    sched = &handle->sched;
    se = &sched->entries[sock->id];
  }
  
  /* TODO: We want to keep a list of out-of-order bumps so
     we can appropriately send each bump to the correct address */
  /* Set IP address and port to socket */
  sock->dst_ip = bump->dst_ip;
  sock->dst_port = bump->dst_port;

  /* Add scheduler entry to the list if it has not been added yet */
  if (se->id == SCHED_ID_INVALID)
  {
    /* For UDP every socket has the same priority */
    ret = sched_add(sched, bump->sock_id, 0, 
        se->avail + bump->tx_avail, (uint64_t) sock);
    if (ret != 0)
    {
      LOG_ERROR("failed to add entry to scheduler");
      return -1;
    }
  }

  return 0;
}
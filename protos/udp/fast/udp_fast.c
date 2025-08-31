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

struct udp_sock * udp_sock_find(struct cham_proto_handle *handle,
    uint32_t remote_ip_be, uint16_t remote_port_be);

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

/* TODO: For now just return the first socket always */
struct udp_sock *udp_sock_find(struct cham_proto_handle *handle,
    uint32_t remote_ip_be, uint16_t remote_port_be)
{
  struct udp_sock *sock_map;
  sock_map = handle->maps[SOCK_MAP_IDX].addr;
  return &sock_map[0];
}

int udp_init_fp(void *config)
{
  return 0;
}

int udp_event_rx(void *pkt, struct cham_proto_handle *handle)
{
  int ret;
  uint16_t ip_hdrs_len, ip_total_len, udp_len, payload_len;
  uint64_t mac_src_val, mac_dst_val;
  struct eth_hdr *eth;
  struct ip_hdr  *ip;
  struct udp_hdr *udp;
  void *payload;

  struct udp_sock *sock;
  struct equeue *q;
  struct udp_queue_entry *qe;
  struct udp_queue_bump *bump;

  uint8_t *rx_base;
  uint32_t free_bytes;
  uint32_t tail;
  uint32_t part;

  uint16_t ip_saved_chksum, ip_comp_chksum;
  uint16_t udp_saved_chksum, udp_comp_chksum;

  sock = NULL;

  /* Parse ETH header */
  eth = (struct eth_hdr *) pkt;
  if (f_beui16(eth->type) != ETH_TYPE_IP)
  {
    LOG_ERROR("rx drop: non-IPv4 ethertype=%04x", f_beui16(eth->type));
    return -1;
  }
  memcpy(&mac_src_val, &eth->src, ETH_ADDR_LEN);
  memcpy(&mac_dst_val, &eth->dst, ETH_ADDR_LEN);

  /* Parse IP header */
  ip = (struct ip_hdr *) ((uint8_t *) pkt + sizeof(struct eth_hdr));
  if (IPH_V(ip) != 4 || IPH_HL(ip) < 5)
  {
    LOG_ERROR("rx drop: bad IPv4 header v=%u hl=%u", IPH_V(ip), IPH_HL(ip));
    return -1;
  }

  ip_hdrs_len  = (uint16_t) (IPH_HL(ip) * 4);
  ip_total_len = f_beui16(ip->len);

  if (ip->proto != IP_PROTO_UDP)
  {
    LOG_ERROR("rx drop: proto=%u != UDP", ip->proto);
    return -1;
  }

  /* Drop fragmented IPv4 for now */
  if (f_beui16(ip->offset) & 0x3FFF)
  {
    LOG_ERROR("rx drop: fragmented packet (offset=%04x)", f_beui16(ip->offset));
    return -1;
  }

  if (ip_total_len < ip_hdrs_len + (uint16_t) sizeof(struct udp_hdr))
  {
    LOG_ERROR("rx drop: malformed lengths ip_total=%u ip_hl=%u",
              ip_total_len, ip_hdrs_len);
    return -1;
  }

  /* Verify IPv4 header checksum */
  ip_saved_chksum = ip->chksum;
  ip->chksum = 0;
  ip_comp_chksum = rte_ipv4_cksum((void *) ip);
  ip->chksum = ip_saved_chksum;
  if (ip_comp_chksum != ip_saved_chksum)
  {
    LOG_ERROR("rx drop: bad IPv4 checksum computed=%04x saved=%04x",
              ip_comp_chksum, ip_saved_chksum);
    return -1;
  }

  /* Parse UDP header */
  udp = (struct udp_hdr *) ((uint8_t *) ip + ip_hdrs_len);
  udp_len = f_beui16(udp->len);
  if (udp_len < sizeof(struct udp_hdr))
  {
    LOG_ERROR("rx drop: bad UDP len=%u", udp_len);
    return -1;
  }
  
  if (ip_total_len < ip_hdrs_len + udp_len)
  {
    LOG_ERROR("rx drop: IP shorter than UDP (ip_total=%u need=%u)",
              ip_total_len, ip_hdrs_len + udp_len);
    return -1;
  }

  /* Verify UDP checksum for IPv4 (0 means “no checksum”) */
  udp_saved_chksum = udp->chksum;
  if (udp_saved_chksum != 0)
  {
    udp->chksum = 0;
    udp_comp_chksum = rte_ipv4_udptcp_cksum((void *) ip, (void *) udp);
    udp->chksum = udp_saved_chksum;
    
    if (udp_comp_chksum != udp_saved_chksum)
    {
      LOG_ERROR("rx drop: bad UDP checksum computed=%04x saved=%04x",
                udp_comp_chksum, udp_saved_chksum);
      return -1;
    }
  }

  /* Lookup socket */
  uint32_t src_ip_be  = htonl(f_beui32(ip->src));
  uint16_t src_prt_be = htons(f_beui16(udp->src));
  sock = udp_sock_find(handle, src_ip_be, src_prt_be);
  
  if (sock == NULL)
  {
    LOG_ERROR("rx drop: no socket for src=%08x:%u",
              f_beui32(ip->src), f_beui16(udp->src));
    return -1;
  }

  /* Copy payload */
  payload_len = (uint16_t) (udp_len - sizeof(struct udp_hdr));
  payload = (void *) ((uint8_t *) udp + sizeof(struct udp_hdr));
  rx_base = (uint8_t *) handle->shm_base + sock->rx_off;
  free_bytes = sock->rx_len - sock->rx_avail;

  if (payload_len > free_bytes)
  {
    /* Drop whole datagram if it doesn’t fit */
    // LOG_ERROR("rx drop: ring full (need=%u free=%u)", payload_len, free_bytes);
    return -1;
  }
  
  tail = sock->rx_head + sock->rx_avail;
  if (tail >= sock->rx_len)
    tail -= sock->rx_len;

  if (tail + payload_len <= sock->rx_len)
  {
    memcpy(rx_base + tail, payload, payload_len);
  }
  else
  {
    part = sock->rx_len - tail;
    memcpy(rx_base + tail, payload, part);
    memcpy(rx_base, (uint8_t *) payload + part, payload_len - part);
  }

  /* Publish bytes for the consumer */
  sock->rx_avail += payload_len;

  /* Send bump to applocation */
  q = &handle->equeues[sock->app_bump_qid].eq;
  qe = queue_tail(q);
  if (qe == NULL)
  {
    LOG_ERROR("failed to get bump queue tail");
    return -1;
  }

  bump = &qe->data.bump;
  bump->opaque   = sock->opaque;
  bump->rx_avail = payload_len;
  bump->rx_head  = 0;
  bump->tx_avail = 0;
  bump->tx_head  = 0;

  ret = queue_enqueue(q, UDP_QUEUE_BUMP);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue bump message");
    return -1;
  }

  // LOG_DEBUG("rx udp: src_port=%d dst_port=%d",
  //           f_beui16(udp->src), f_beui16(udp->dst));
  // LOG_DEBUG("rx ip: src_ip=%08x dst_ip=%08x",
  //           f_beui32(ip->src), f_beui32(ip->dst));
  // LOG_DEBUG("rx eth: src_mac=%012" PRIx64 " dst_mac=%012" PRIx64,
  //           (uint64_t)(be64toh(mac_src_val)),
  //           (uint64_t)(be64toh(mac_dst_val)));

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
  struct cham_sched_entry *se;
  uint16_t opt_len, payload_len;
  uint16_t udp_hdrs_len, ip_hdrs_len, pkt_hdrs_len;
  uint32_t new_head;
  uint64_t mac_src_val, mac_dst_val, part;
  struct eth_addr mac_src, mac_dst;
  struct in_addr ip_src, ip_dst;
  uint32_t ip_src_be, ip_dst_be;
  struct udp_pkt *p = (struct udp_pkt *) pkt;
  
  /* If there is nothing scheduled return */
  sched = &handle->sched;
  se = sched_head(sched);
  if (se == NULL)
    return -1;
  sock = (struct udp_sock *) se->opaque;
    
  /* Calculate number of bytes to transmit */
  payload_len = se->avail;
  if (payload_len > UDP_MSS)
    payload_len = UDP_MSS;

  /* TODO: Opt len */
  opt_len = 0;
  udp_hdrs_len = sizeof(struct udp_hdr) + opt_len;
  ip_hdrs_len = sizeof(struct ip_hdr);
  pkt_hdrs_len = sizeof(struct eth_hdr) + sizeof(struct ip_hdr)
    + sizeof(struct udp_hdr) + opt_len;

  /* Set ETH header */
  mac_from_text("b8:59:9f:c4:af:e6", mac_src.addr);
  mac_from_text("b8:59:9f:c4:af:66", mac_dst.addr);

  p->eth.src = mac_src;
  p->eth.dst = mac_dst;
  p->eth.type = t_beui16(ETH_TYPE_IP);
  memcpy(&mac_src_val, &p->eth.src, ETH_ADDR_LEN);
  memcpy(&mac_dst_val, &p->eth.dst, ETH_ADDR_LEN);

  /* Set IP header */
  // ip_dst_be = sock->dst_ip;
  // /* TODO: set actual IP*/
  // inet_pton(AF_INET, "192.168.10.14", &ip_src);
  // // ip_src_be = (uint32_t) ip_src.s_addr;
  inet_pton(AF_INET, "192.168.10.13", &ip_src);
  inet_pton(AF_INET, "192.168.10.14", &ip_dst);
  ip_src_be = (uint32_t) ip_src.s_addr;
  ip_dst_be = (uint32_t ) ip_dst.s_addr;

  IPH_VHL_SET(&p->ip, 4, 5);
  p->ip._tos = 0;
  p->ip.len = t_beui16(ip_hdrs_len + udp_hdrs_len + payload_len);
  p->ip.id = t_beui16(3); /* not sure why we have 3 here */
  p->ip.offset = t_beui16(0);
  p->ip.ttl = 0xff;
  p->ip.proto = IP_PROTO_UDP;
  p->ip.src = t_beui32(ntohl(ip_src_be));
  p->ip.dst = t_beui32(ntohl(ip_dst_be));
  p->ip.chksum = 0;

  /* Set UDP header */
  // p->udp.dst = t_beui16(sock->dst_port);
  // /* TODO: Add actual port */
  // p->udp.src = t_beui16(1234);
  p->udp.src = t_beui16(1234);
  p->udp.dst = t_beui16(1234);
  p->udp.len = t_beui16(udp_hdrs_len + payload_len);
  p->udp.chksum = 0; /* UDP checksum has to be 0 before we compute it */
  
  /* Copy data to packet */
  payload = pkt + pkt_hdrs_len;
  if (sock->tx_head + payload_len <= sock->tx_len) 
  {
    memcpy(payload, handle->shm_base + sock->tx_off + sock->tx_head, payload_len);
  } 
  else 
  {
    part = sock->tx_len - sock->tx_head;
    memcpy(payload, handle->shm_base + sock->tx_off + sock->tx_head, part);
    memcpy(payload + part, handle->shm_base + sock->tx_off, payload_len - part);
  }
  
  /* Compute checksums */
  p->udp.chksum = rte_ipv4_udptcp_cksum((void *) &p->ip, (void *) &p->udp);
  p->ip.chksum = rte_ipv4_cksum((void *) &p->ip);
  
  /* Update socket and schduler structs */
  new_head = sock->tx_head + payload_len;
  if (new_head >= sock->tx_len)
    new_head -= sock->tx_len;
  sock->tx_head = new_head;
  sock->tx_avail -= payload_len;
  se->avail -= payload_len;
  se->opaque = (uint64_t) sock;

  /* Remove first element from priority list */
  ret = sched_pop(sched);
  if (ret != 0)
  {
    LOG_ERROR("failed to pop entry from scheduler");
    return -1;
  }
  
  /* Add entry to the back if there is still data to send */
  if (se->avail > 0)
  {
    ret = sched_add(sched, sock->id, 0);
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

  // LOG_DEBUG("tx udp: src_port=%d dst_port=%d",
  //     f_beui16(p->udp.src), f_beui16(p->udp.dst));
  // LOG_DEBUG("tx ip: dst_ip=%08x src_ip=%08x",
  //     f_beui32(p->ip.dst), f_beui32(p->ip.src));
  // LOG_DEBUG("tx eth: src_mac=%012" PRIx64 " dst_mac=%012" PRIx64,
  //         (uint64_t)(be64toh(mac_src_val)),
  //         (uint64_t)(be64toh(mac_dst_val)));

  return pkt_hdrs_len + payload_len;
}

int udp_event_deq(int qid, struct queue_entry *qe,
  struct cham_proto_handle *handle)
{
  int ret;
  uint32_t new_head;
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
  
  new_head = sock->rx_head + bump->rx_head;
  if (new_head >= sock->rx_len)
    new_head -= sock->rx_len;
  sock->rx_head = new_head;
  sock->rx_avail -= bump->rx_head;

  /* TODO: We want to keep a list of out-of-order bumps so
     we can appropriately send each bump to the correct address */
  /* Set IP address and port to socket */
  sock->dst_ip = bump->dst_ip;
  sock->dst_port = bump->dst_port;
  
  sched = &handle->sched;
  se = &sched->entries[sock->id];
  se->avail = se->avail + bump->tx_avail;
  se->opaque = (uint64_t) sock;

  /* Add scheduler entry to the list if it has not been added yet */
  if (se->id == SCHED_ID_INVALID)
  {
    /* For UDP every socket has the same priority */
    ret = sched_add(sched, bump->sock_id, 0);
    if (ret != 0)
    {
      LOG_ERROR("failed to add entry to scheduler");
      return -1;
    }
  }

  return 0;
}
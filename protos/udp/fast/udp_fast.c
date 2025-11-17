#include <rte_ip4.h>
#include <cham_fast.h>

#include "scheduler_fns.h"
#include "udp.h"
#include "udp_fast.h"
#include "udp_queue_types.h"
#include "queue_fns.h"
#include "queue_types.h"
#include "log.h"

#define SOCK_MAP_IDX 0

int mac_from_text(const char *text, __u8 out[ETH_ADDR_LEN]);
int handle_bump_tx(struct udp_queue_bump_entry *qe, 
    struct cham_proto_handle *handle);
int handle_bump_rx(struct udp_queue_bump_entry *qe, 
    struct cham_proto_handle *handle);

struct udp_sock * udp_sock_find(struct cham_map *maps,
    __u16 local_port);

int udp_event_rx(void *pkt, struct cham_proto_handle *handle)
{
  int ret;
  __u16 ip_hdrs_len, ip_total_len, udp_len, payload_len;
  struct eth_hdr *eth;
  struct ip_hdr  *ip;
  struct udp_hdr *udp;
  void *payload;

  struct udp_sock *sock;
  struct equeue *q;
  struct udp_queue_bump_entry *qe;
  struct udp_queue_bump_app_rx *bump;

  __u8 *rx_base;
  __u32 free_bytes;
  __u32 tail;
  __u32 part;

  __u16 ip_saved_chksum, ip_comp_chksum;
  __u16 udp_saved_chksum, udp_comp_chksum;

  sock = NULL;

  /* Parse ETH header */
  eth = (struct eth_hdr *) pkt;
  if (f_beui16(eth->type) != ETH_TYPE_IP)
  {
    LOG_ERROR("rx drop: non-IPv4 ethertype=%04x", f_beui16(eth->type));
    return -1;
  }

  /* Parse IP header */
  ip = (struct ip_hdr *) ((__u8 *) pkt + sizeof(struct eth_hdr));
  if (IPH_V(ip) != 4 || IPH_HL(ip) < 5)
  {
    LOG_ERROR("rx drop: bad IPv4 header v=%u hl=%u", IPH_V(ip), IPH_HL(ip));
    return -1;
  }

  ip_hdrs_len  = (__u16) (IPH_HL(ip) * 4);
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

  if (ip_total_len < ip_hdrs_len + (__u16) sizeof(struct udp_hdr))
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
  udp = (struct udp_hdr *) ((__u8 *) ip + ip_hdrs_len);
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
  sock = udp_sock_find(handle->maps, f_beui16(udp->dst));
  if (sock == NULL)
  {
    /* Socket doesn't exist so send to slow-path */
    /* TODO: Send socket to slow-path */
    LOG_ERROR("rx drop: no socket for src=%08x:%u",
              f_beui32(ip->src), f_beui16(udp->src));
    return -1;
  }

  /* Copy payload */
  payload_len = (__u16) (udp_len - sizeof(struct udp_hdr));
  payload = (void *) ((__u8 *) udp + sizeof(struct udp_hdr));
  rx_base = (__u8 *) handle->shm_base + sock->rx_off;
  free_bytes = sock->rx_len - sock->rx_avail;

  if (payload_len > free_bytes)
    return -1;
  
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
    memcpy(rx_base, (__u8 *) payload + part, payload_len - part);
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

  bump = &qe->data.bump_app_rx;
  bump->opaque   = sock->opaque;
  bump->rx_avail = payload_len;
  bump->rx_port = f_beui16(udp->src);
  bump->rx_ip = f_beui32(ip->src);

  ret = queue_enqueue(q, UDP_QUEUE_BUMP_APP_RX);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue bump message");
    return -1;
  }

  return 0;
}

int udp_event_tx(void *pkt, struct cham_proto_handle *handle)
{
  int ret;
  void *payload;
  struct udp_sock *sock;
  struct equeue *q;
  struct udp_queue_bump_entry *qe;
  struct udp_queue_bump_app_tx *bump;
  struct cham_scheduler *sched;
  struct cham_sched_entry *se;
  __u16 opt_len, payload_len;
  __u16 udp_hdrs_len, ip_hdrs_len, pkt_hdrs_len;
  __u32 new_head;
  __u64 part;
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
  // mac_from_text("b8:59:9f:c4:af:e6", mac_src.addr);
  p->eth.src.addr[0] = 184;
  p->eth.src.addr[1] = 89;
  p->eth.src.addr[2] = 159;
  p->eth.src.addr[3] = 196;
  p->eth.src.addr[4] = 175;
  p->eth.src.addr[5] = 230;
  
  // mac_from_text("b8:59:9f:c4:af:66", mac_dst.addr);
  p->eth.dst.addr[0] = 184;
  p->eth.dst.addr[1] = 89;
  p->eth.dst.addr[2] = 159;
  p->eth.dst.addr[3] = 196;
  p->eth.dst.addr[4] = 175;
  p->eth.dst.addr[5] = 102;

  p->eth.type = t_beui16(ETH_TYPE_IP);
  // memcpy(&mac_src_val, &p->eth.src, ETH_ADDR_LEN);
  // memcpy(&mac_dst_val, &p->eth.dst, ETH_ADDR_LEN);

  /* Set IP header */
  IPH_VHL_SET(&p->ip, 4, 5);
  p->ip._tos = 0;
  p->ip.len = t_beui16(ip_hdrs_len + udp_hdrs_len + payload_len);
  p->ip.id = t_beui16(3); /* not sure why we have 3 here */
  p->ip.offset = t_beui16(0);
  p->ip.ttl = 0xff;
  p->ip.proto = IP_PROTO_UDP;
  p->ip.src = t_beui32(sock->local_ip);
  p->ip.dst = t_beui32(sock->remote_ip);
  p->ip.chksum = 0;

  /* Set UDP header */
  p->udp.src = t_beui16(sock->local_port);
  p->udp.dst = t_beui16(sock->remote_port);
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
  se->opaque = (__u64) sock;

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

  bump = &qe->data.bump_app_tx;
  bump->opaque = sock->opaque;
  bump->tx_head = payload_len;

  ret = queue_enqueue(q, UDP_QUEUE_BUMP_APP_TX);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue bump message");
    return -1;
  }

  // LOG_DEBUG("tx udp: src_port=%d dst_port=%d",
      // f_beui16(p->udp.src), f_beui16(p->udp.dst));
  // LOG_DEBUG("tx ip: dst_ip=%08x src_ip=%08x",
      // f_beui32(p->ip.dst), f_beui32(p->ip.src));
  // LOG_DEBUG("tx eth: src_mac=%012" PRIx64 " dst_mac=%012" PRIx64,
  //         (__u64)(be64toh(mac_src_val)),
  //         (__u64)(be64toh(mac_dst_val)));

  return pkt_hdrs_len + payload_len;
}

int udp_event_deq(int qid, struct queue_entry *qe,
  struct cham_proto_handle *handle)
{
  int ret;
  struct udp_queue_bump_entry *udp_qe;

  udp_qe = (struct udp_queue_bump_entry *) qe;
  
  switch (qe->type)
  {
    case UDP_QUEUE_BUMP_CHAM_TX:
      ret = handle_bump_tx(udp_qe, handle);
      break;
    case UDP_QUEUE_BUMP_CHAM_RX:
      ret = handle_bump_rx(udp_qe, handle);
      break;
    default:
      LOG_ERROR("unknown bump queue entry");
      ret = -1;
  }

  return ret;
}

int handle_bump_tx(struct udp_queue_bump_entry *qe, 
    struct cham_proto_handle *handle)
{
  int ret;
  struct udp_sock *sock, *sock_map;
  struct cham_scheduler *sched;
  struct cham_sched_entry *se;
  struct udp_queue_bump_cham_tx *bump;
  
  sock_map = handle->maps[SOCK_MAP_IDX].addr;
  bump = &qe->data.bump_cham_tx;
  sock = &sock_map[bump->sock_id];
  sock->tx_avail += bump->tx_avail;

  /* TODO: We want to keep a list of out-of-order bumps so
    we can appropriately send each bump to the correct address */
  /* Set IP address and port to socket */
  sock->remote_ip = bump->tx_ip;
  sock->remote_port = bump->tx_port;
  
  sched = &handle->sched;
  se = &sched->entries[sock->id];
  se->avail = se->avail + bump->tx_avail;
  se->opaque = (__u64) sock;

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

int handle_bump_rx(struct udp_queue_bump_entry *qe,
    struct cham_proto_handle *handle)
{
  __u32 new_head;
  struct udp_sock *sock, *sock_map;
  struct udp_queue_bump_cham_rx *bump;
  
  sock_map = handle->maps[SOCK_MAP_IDX].addr;
  bump = &qe->data.bump_cham_rx;
  sock = &sock_map[bump->sock_id];
  
  new_head = sock->rx_head + bump->rx_head;
  if (new_head >= sock->rx_len)
    new_head -= sock->rx_len;
  sock->rx_head = new_head;
  sock->rx_avail -= bump->rx_head;

  return 0;
}

/* TODO: For now just return the first socket always */
struct udp_sock *udp_sock_find(struct cham_map *maps,
    __u16 local_port)
{
  struct udp_sock *sock_map;

  if (local_port < 1 || local_port > 65535)
    return NULL;

  sock_map = maps[SOCK_MAP_IDX].addr;
  return &sock_map[local_port];
}
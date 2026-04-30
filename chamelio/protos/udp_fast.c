#include <string.h>
#include <linux/types.h>

#include <rte_ip4.h>

#include "queue_fns.h"

#include "cham_fast.h"
#include "udp_hdr.h"
#include "ip_hdr.h"
#include "udp.h"
#include "udp_queue_types.h"
#include "utils.h"
#include "log.h"

#define PORT_MAP 0
#define SOCK_MAP 1
#define REUPORT_MAP 2
#define CFG_MAP 2
#define UDP_MAX_PAYLOAD (FAST_L3_PKT_ROOM - sizeof(struct udp_pkt_inner))

static inline struct udp_sock * udp_sock_find(struct cham_ebpf_ctx *ctx,
    __u16 local_port);
static inline __u16 find_free_port(struct cham_ebpf_ctx *ctx);
static inline int handle_bump_rx(struct cham_ebpf_ctx *ctx);
static inline int handle_bump_tx(struct cham_ebpf_ctx *ctx);


int udp_event_rx(struct cham_ebpf_ctx *ctx)
{
  int ret;
  __u16 ip_hdrs_len, ip_total_len, udp_len, payload_len;
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

  sock = NULL;
  
  /* Parse IP header */
  if (ctx->pkt + sizeof(struct ip_hdr) > ctx->pkt_end)
    return -1;

  ip = (struct ip_hdr *) ctx->pkt;
  if (IPH_V(ip) != 4 || IPH_HL(ip) < 5)
    return -1;

  if (ip->proto != IP_PROTO_UDP)
    return -1;

  /* Drop fragmented IPv4 for now */
  if (f_beui16(ip->offset) & 0x3FFF)
    return -1;

  ip_hdrs_len  = (__u16) (IPH_HL(ip) * 4);
  ip_total_len = f_beui16(ip->len);

  if (ip_total_len < ip_hdrs_len + (__u16) sizeof(struct udp_hdr))
    return -1;

  /* Parse UDP header */
  if ((__u8 *) ip + ip_hdrs_len + sizeof(struct udp_hdr) >
      (__u8 *) ctx->pkt_end)
    return -1;

  udp = (struct udp_hdr *) ((__u8 *) ip + ip_hdrs_len);
  udp_len = f_beui16(udp->len);
  
  if (udp_len < sizeof(struct udp_hdr))
    return -1;
  
  if (ip_total_len < ip_hdrs_len + udp_len)
    return -1;

  /* Lookup socket */
  sock = udp_sock_find(ctx, f_beui16(udp->dst));
  
  /* Socket doesn't exist so drop it */
  if (sock == NULL)
    return -1;

  /* Copy payload */
  payload_len = (__u16) (udp_len - sizeof(struct udp_hdr));
  payload = (void *) ((__u8 *) udp + sizeof(struct udp_hdr));

  rx_base = (__u8 *) ctx->shm_base + sock->rx_off;
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

  /* Update number of available bytes */
  sock->rx_avail += payload_len;
  
  /* Send bump to application */
  q = &ctx->equeues[sock->app_bump_qid].eq;
  qe = queue_tail(q);
  if (qe == NULL)
    return -1;

  bump = &qe->data.bump_app_rx;
  bump->opaque   = sock->opaque;
  bump->rx_avail = payload_len;
  bump->rx_port = f_beui16(udp->src);
  bump->rx_ip = f_beui32(ip->src);

  ret = queue_enqueue(q, UDP_QUEUE_BUMP_APP_RX);
  if (ret != 0)
    return -1;

  return 0;
}

int udp_event_sched(struct cham_ebpf_ctx *ctx)
{
  return -1;
}

int udp_event_deq(struct cham_ebpf_ctx *ctx)
{
  int ret;
  
  if ((__u8 *) ctx->qe + sizeof(*ctx->qe) > (__u8 *) ctx->shm_end)
    return -1;

  switch (ctx->qe->type)
  {
    case UDP_QUEUE_BUMP_CHAM_TX:
      return handle_bump_tx(ctx);
    case UDP_QUEUE_BUMP_CHAM_RX:
      return handle_bump_rx(ctx);
    default:
      return -1;
  }

  return ret;
}

static inline int handle_bump_tx(struct cham_ebpf_ctx *ctx)
{
  int ret;
  void *payload;
  struct udp_port *port;
  struct udp_sock *sock;
  struct equeue *q;
  struct udp_queue_bump_entry *qe;
  struct udp_queue_bump_cham_tx *bump_cham;
  struct udp_queue_bump_app_tx *bump_app;
  struct cham_map *map;
  __u16 opt_len, payload_len, local_port;
  __u16 udp_hdrs_len, ip_hdrs_len, pkt_hdrs_len;
  __u32 max_payload;
  __u32 new_head;
  __u64 part;

  /* Perform bounds check so eBPF is happy */
  struct udp_pkt_inner *p = (struct udp_pkt_inner *) ctx->pkt;
  if ((__u8 *) p + sizeof(struct udp_pkt_inner) > (__u8 *) ctx->pkt_end)
    return -1;

  qe = (struct udp_queue_bump_entry *) ctx->qe;
  bump_cham = &qe->data.bump_cham_tx;

  map = &ctx->maps[SOCK_MAP];
  sock = (struct udp_sock *) ((__u8 *) map->addr +
      (bump_cham->sock_id * sizeof(struct udp_sock)));
  if (sock == NULL)
    return -1;

  /* Set local port for socket if it is not defined yet */
  if (sock->local_port == 0)
  {
    local_port = find_free_port(ctx);
    if (local_port == 0)
      return -1;
    map = &ctx->maps[PORT_MAP];
    port = (struct udp_port *) ((__u8 *) map->addr +
        (local_port * sizeof(struct udp_port)));
    if (port == NULL)
      return -1;

    port->sids[0] = sock->id;
    port->nsocks++;
    sock->local_port = local_port;
  }

  /* Calculate number of bytes to transmit */
  payload_len = bump_cham->tx_avail;
  max_payload = (__u32) ((__u8 *) ctx->pkt_end -
      ((__u8 *) p + sizeof(struct udp_pkt_inner)));
  if (payload_len > UDP_MAX_PAYLOAD)
    payload_len = UDP_MAX_PAYLOAD;
  if (payload_len > max_payload)
    payload_len = max_payload;

  /* Drop if payload_len out of bounds */
  if ((__u8 *) p + sizeof(struct udp_pkt_inner) + payload_len >
      (__u8 *) ctx->pkt_end)
    return -1;

  opt_len = 0;
  udp_hdrs_len = sizeof(struct udp_hdr) + opt_len;
  ip_hdrs_len = sizeof(struct ip_hdr);
  pkt_hdrs_len = ip_hdrs_len + udp_hdrs_len;

  /* Set IP header */
  IPH_VHL_SET(&p->ip, 4, 5);
  p->ip._tos = 0;
  p->ip.len = t_beui16(ip_hdrs_len + udp_hdrs_len + payload_len);
  p->ip.id = t_beui16(3); /* not sure why we have 3 here */
  p->ip.offset = t_beui16(0);
  p->ip.ttl = 0xff;
  p->ip.proto = IP_PROTO_UDP;
  p->ip.src = t_beui32(sock->local_ip);
  p->ip.dst = t_beui32(bump_cham->tx_ip);
  p->ip.chksum = 0;

  /* Set UDP header */
  p->udp.src = t_beui16(sock->local_port);
  p->udp.dst = t_beui16(bump_cham->tx_port);
  p->udp.len = t_beui16(udp_hdrs_len + payload_len);

  /* Copy data to packet */
  payload = ctx->pkt + pkt_hdrs_len;
  if (sock->tx_head + payload_len <= sock->tx_len) 
  {
    memcpy(payload, ctx->shm_base + sock->tx_off + sock->tx_head, payload_len);
  } 
  else 
  {
    part = sock->tx_len - sock->tx_head;
    memcpy(payload, ctx->shm_base + sock->tx_off + sock->tx_head, part);
    memcpy(payload + part, ctx->shm_base + sock->tx_off, payload_len - part);
  }
  
  /* Update socket */
  sock->tx_avail += bump_cham->tx_avail;
  new_head = sock->tx_head + payload_len;
  if (new_head >= sock->tx_len)
    new_head -= sock->tx_len;
  sock->tx_head = new_head;
  sock->tx_avail -= payload_len;

  /* Send a bump to application */
  q = &ctx->equeues[sock->app_bump_qid].eq;
  qe = (struct udp_queue_bump_entry *) queue_tail(q);
  if (qe == NULL)
    return -1;

  bump_app = &qe->data.bump_app_tx;
  bump_app->opaque = sock->opaque;
  bump_app->tx_head = payload_len;

  ret = queue_enqueue(q, UDP_QUEUE_BUMP_APP_TX);
  if (ret != 0)
    return -1;

  return pkt_hdrs_len + payload_len;
}
 
static inline int handle_bump_rx(struct cham_ebpf_ctx *ctx)
{
  __u32 new_head;
  struct udp_sock *sock;
  struct udp_queue_bump_cham_rx *bump;
  struct udp_queue_bump_entry *qe;
  struct cham_map *map;

  qe = (struct udp_queue_bump_entry *) ctx->qe;
  if ((__u8 *) qe + sizeof(struct udp_queue_bump_entry) >
      (__u8 *) ctx->shm_end)
    return -1;
  bump = &qe->data.bump_cham_rx;

  map = &ctx->maps[SOCK_MAP];
  sock = (struct udp_sock *) ((__u8 *) map->addr +
      (bump->sock_id * sizeof(struct udp_sock)));
  if (sock == NULL)
    return -1;
  
  new_head = sock->rx_head + bump->rx_head;
  if (new_head >= sock->rx_len)
    new_head -= sock->rx_len;
  sock->rx_head = new_head;
  sock->rx_avail -= bump->rx_head;

  return 0;
}

static inline __u16 find_free_port(struct cham_ebpf_ctx *ctx)
{
  __u16 i, nr, next_port;
  struct udp_cfg *cfg;
  struct udp_port *port;
  struct cham_map *map;

  map = &ctx->maps[CFG_MAP];
  cfg = (struct udp_cfg *) map->addr;
  if (cfg == NULL)
    return 0;

  next_port = cfg->next_port;
  if (next_port < MIN_PORT || next_port > MAX_PORT)
    next_port = MIN_PORT;

  map = &ctx->maps[PORT_MAP];
  for (i = 0; i < UDP_PORT_SCAN_MAX; i++)
  {
    nr = next_port + i;
    if (nr > MAX_PORT)
      nr = MIN_PORT + nr - MAX_PORT - 1;

    port = (struct udp_port *) ((__u8 *) map->addr +
        (nr * sizeof(struct udp_port)));
    if (port == NULL)
      return 0;

    if (port->nsocks == 0)
    {
      cfg->next_port = nr + 1;
      if (cfg->next_port > MAX_PORT)
        cfg->next_port = MIN_PORT;
      return nr;
    }
  }

  return 0;
}

static inline struct udp_sock *udp_sock_find(struct cham_ebpf_ctx *ctx,
    __u16 local_port)
{
  struct udp_port *port;
  __u16 sock_id;
  struct cham_map *map;

  if (local_port < MIN_PORT || local_port > 65535)
    return NULL;

  map = &ctx->maps[PORT_MAP];
  port = (struct udp_port *) ((__u8 *) map->addr +
      (local_port * sizeof(struct udp_port)));
  if (port == NULL || port->nsocks == 0)
    return NULL;
    
  /* Hash src port to one of the sockets if reusable port */  
  if (port->nsocks < 2)
  {
    sock_id = port->sids[0];
  }
  else
  {
    if (port->next_sock < 0 || port->next_sock >= MAX_REUSOCK_PORT)
      return NULL;
    sock_id = port->sids[port->next_sock];
    port->next_sock = (port->next_sock + 1) % port->nsocks;
  }
  
  map = &ctx->maps[SOCK_MAP];
  return (struct udp_sock *) ((__u8 *) map->addr +
      (sock_id * sizeof(struct udp_sock)));
}

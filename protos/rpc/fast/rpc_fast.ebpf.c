#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

#include "cham_fast.h"
#include "udp_hdr.h"
#include "ip_hdr.h"
#include "eth_hdr.h"
#include "rpc.h"
#include "rpc_fast.h"
#include "rpc_queue_types.h"
#include "utils.h"

#define PORT_MAP 0
#define SOCK_MAP 1
#define REUPORT_MAP 2

static __always_inline struct rpc_sock * rpc_sock_find(struct cham_map *maps,
    __u16 local_port);
static __always_inline __u16 find_free_port(struct cham_ebpf_ctx *ctx);
static __always_inline int handle_bump_rx(struct cham_ebpf_ctx *ctx);
static __always_inline int handle_bump_tx(struct cham_ebpf_ctx *ctx);
    
/* Add these functions as helpers */
static void * (*queue_tail)(struct equeue *q) = (void *) 1001;
static int (*queue_enqueue)(struct equeue *q, __u8 type) = (void *) 1002;

static void * (*bpf_memcpy)(void *dst, void *src, size_t len) = (void *) 1003;
static void (*bpf_print)(int a) = (void *) 1004;

static __u16 (*ipv4_checksum)(void *ip_hdr) = (void *) 1005;
static __u16 (*ipv4_udptcp_cksum)(void *ip_hdr, void *udp_hdr) = (void *) 1006;

static struct cham_sched_entry * (*sched_head)(struct cham_scheduler *sched) = (void *) 1007;
static int (*sched_pop)(struct cham_scheduler *sched) = (void *) 1008;
static int (*sched_add)(struct cham_scheduler *sched, __u32 id, __u32 priority) = (void *) 1009;


SEC("chamelio/event_rx")
int event_rx(struct cham_ebpf_ctx *ctx)
{
int ret;
  __u16 ip_hdrs_len, ip_total_len, udp_len, payload_len;
  struct eth_hdr *eth;
  struct ip_hdr  *ip;
  struct udp_hdr *udp;
  void *payload, *pkt;

  struct rpc_sock *sock;
  struct equeue *q;
  struct rpc_queue_bump_entry *qe;
  struct rpc_queue_bump_app_rx *bump;

  __u8 *rx_base;
  __u32 free_bytes;
  __u32 tail;
  __u32 part;

  __u16 ip_saved_chksum, ip_comp_chksum;
  __u16 udp_saved_chksum, udp_comp_chksum;

  sock = NULL;
  pkt = ctx->pkt;
  
  /* Parse ETH header */
  eth = (struct eth_hdr *) pkt;
  if (f_beui16(eth->type) != ETH_TYPE_IP)
    return -1;
  
  /* Parse IP header */
  ip = (struct ip_hdr *) ((__u8 *) pkt + sizeof(struct eth_hdr));
  if (IPH_V(ip) != 4 || IPH_HL(ip) < 5)
    return -1;

  ip_hdrs_len  = (__u16) (IPH_HL(ip) * 4);
  ip_total_len = f_beui16(ip->len);

  if (ip->proto != IP_PROTO_UDP)
    return -1;

  /* Drop fragmented IPv4 for now */
  if (f_beui16(ip->offset) & 0x3FFF)
    return -1;

  if (ip_total_len < ip_hdrs_len + (__u16) sizeof(struct udp_hdr))
    return -1;

  /* Verify IPv4 header checksum */
  ip_saved_chksum = ip->chksum;
  ip->chksum = 0;
  ip_comp_chksum = ipv4_checksum((void *) ip);
  ip->chksum = ip_saved_chksum;
  if (ip_comp_chksum != ip_saved_chksum)
    return -1;

  /* Parse UDP header */
  udp = (struct udp_hdr *) ((__u8 *) ip + ip_hdrs_len);
  udp_len = f_beui16(udp->len);
  if (udp_len < sizeof(struct udp_hdr))
    return -1;
  
  if (ip_total_len < ip_hdrs_len + udp_len)
    return -1;

  /* Verify UDP checksum for IPv4 (0 means “no checksum”) */
  udp_saved_chksum = udp->chksum;
  if (udp_saved_chksum != 0)
  {
    udp->chksum = 0;
    udp_comp_chksum = ipv4_udptcp_cksum((void *) ip, (void *) udp);
    udp->chksum = udp_saved_chksum;
    
    if (udp_comp_chksum != udp_saved_chksum)
    return -1;
  }

  /* Lookup socket */
  sock = rpc_sock_find(ctx->maps, f_beui16(udp->dst));
  
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
    bpf_memcpy(rx_base + tail, payload, payload_len);
  }
  else
  {
    part = sock->rx_len - tail;
    bpf_memcpy(rx_base + tail, payload, part);
    bpf_memcpy(rx_base, (__u8 *) payload + part, payload_len - part);
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

  ret = queue_enqueue(q, RPC_QUEUE_BUMP_APP_RX);
  if (ret != 0)
    return -1;

  return 0;
}

SEC("chamelio/event_tx")
int event_tx(struct cham_ebpf_ctx *ctx)
{
  return -1;
}

SEC("chamelio/event_deq")
int event_deq(struct cham_ebpf_ctx *ctx)
{
  int ret;
  
  switch (ctx->qe->type)
  {
    case RPC_QUEUE_BUMP_CHAM_TX:
      ret = handle_bump_tx(ctx);
      break;
    case RPC_QUEUE_BUMP_CHAM_RX:
      ret = handle_bump_rx(ctx);
      break;
    default:
      ret = -1;
  }

  return ret;
}

static __always_inline int handle_bump_tx(struct cham_ebpf_ctx *ctx)
{
  int ret;
  void *payload;
  struct rpc_port *ports, *port;
  struct rpc_sock *sock, *sock_map;
  struct equeue *q;
  struct rpc_queue_bump_entry *qe;
  struct rpc_queue_bump_cham_tx *bump_cham;
  struct rpc_queue_bump_app_tx *bump_app;
  __u16 opt_len, payload_len, local_port;
  __u16 udp_hdrs_len, ip_hdrs_len, pkt_hdrs_len;
  __u32 new_head;
  __u64 part;
  struct udp_pkt *p = (struct udp_pkt *) ctx->pkt;
  
  qe = (struct rpc_queue_entry *) ctx->qe;
  sock_map = ctx->maps[SOCK_MAP].addr;
  bump_cham = &qe->data.bump_cham_tx;
  sock = &sock_map[bump_cham->sock_id];

  /* Set local port for socket if it is not defined yet */
  if (sock->local_port == 0)
  {
    local_port = find_free_port(ctx);
    if (local_port == 0)
      return -1;
    ports = ctx->maps[PORT_MAP].addr;
    ports[local_port].sids[0] = sock->id;
    ports[local_port].nsocks++;
    sock->local_port = local_port;
  }

  /* Calculate number of bytes to transmit */
  payload_len = bump_cham->tx_avail;
  if (payload_len > UDP_MSS)
    payload_len = UDP_MSS;

  opt_len = 0;
  udp_hdrs_len = sizeof(struct udp_hdr) + opt_len;
  ip_hdrs_len = sizeof(struct ip_hdr);
  pkt_hdrs_len = sizeof(struct eth_hdr) + sizeof(struct ip_hdr)
    + sizeof(struct udp_hdr) + opt_len;

  /* Set ETH header */
  p->eth.type = t_beui16(ETH_TYPE_IP);

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
    bpf_memcpy(payload, ctx->shm_base + sock->tx_off + sock->tx_head, payload_len);
  } 
  else 
  {
    part = sock->tx_len - sock->tx_head;
    bpf_memcpy(payload, ctx->shm_base + sock->tx_off + sock->tx_head, part);
    bpf_memcpy(payload + part, ctx->shm_base + sock->tx_off, payload_len - part);
  }
  
  /* Compute checksums */
  /* UDP checksum has to be 0 before we compute it */
  p->udp.chksum = 0;
  p->udp.chksum = ipv4_udptcp_cksum((void *) &p->ip, (void *) &p->udp);
  p->ip.chksum = ipv4_checksum((void *) &p->ip);
  
  /* Update socket */
  new_head = sock->tx_head + payload_len;
  if (new_head >= sock->tx_len)
    new_head -= sock->tx_len;
  sock->tx_head = new_head;
  sock->tx_avail -= payload_len;

  /* Send a bump to application */
  q = &ctx->equeues[sock->app_bump_qid].eq;
  qe = (struct rpc_queue_entry *) queue_tail(q);
  if (qe == NULL)
    return -1;

  bump_app = &qe->data.bump_app_tx;
  bump_app->opaque = sock->opaque;
  bump_app->tx_head = payload_len;

  ret = queue_enqueue(q, RPC_QUEUE_BUMP_APP_TX);
  if (ret != 0)
    return -1;

  return pkt_hdrs_len + payload_len;
}
 
static __always_inline int handle_bump_rx(struct cham_ebpf_ctx *ctx)
{
  __u32 new_head;
  struct rpc_sock *sock, *sock_map;
  struct rpc_queue_bump_cham_rx *bump;
  struct rpc_queue_bump_entry *qe;
  
  qe = (struct rpc_queue_bump_entry *) ctx->qe;
  sock_map = ctx->maps[SOCK_MAP].addr;
  bump = &qe->data.bump_cham_rx;
  sock = &sock_map[bump->sock_id];
  
  new_head = sock->rx_head + bump->rx_head;
  if (new_head >= sock->rx_len)
    new_head -= sock->rx_len;
  sock->rx_head = new_head;
  sock->rx_avail -= bump->rx_head;

  return 0;
}

static __always_inline __u16 find_free_port(struct cham_ebpf_ctx *ctx)
{
  __u16 i;
  __u16 sock_id;
  struct rpc_port *ports, *port;

  ports = ctx->maps[PORT_MAP].addr;
  for (i = MIN_PORT; i < MAX_SOCKETS; i++)
  {
    port = &ports[i];

    if (port->nsocks == 0)
      return i;
  }

  return 0;
}

static __always_inline struct rpc_sock *rpc_sock_find(struct cham_map *maps,
     __u16 local_port)
{
  struct rpc_port *ports, *port;
  __u16 sock_id, mask;
  struct rpc_sock *sock_map;

  if (local_port < MIN_PORT || local_port > 65535)
    return NULL;

  /* Hash src port to one of the sockets if reusable port */  
  ports = maps[PORT_MAP].addr;
  port = &ports[local_port];
  if (port->nsocks == 0)
    return NULL;

  if (port->nsocks < 2)
  {
    sock_id = port->sids[0];
  }
  else
  {
    sock_id = port->sids[port->next_sock];
    port->next_sock = (port->next_sock + 1) % port->nsocks;
  }

  sock_map = maps[SOCK_MAP].addr;
  return &sock_map[sock_id];
}

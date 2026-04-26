#ifdef CHAM_NATIVE_FAST
#include "native_fast.h"
#else
#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>
#endif

#include "cham_fast.h"
#include "udp_hdr.h"
#include "ip_hdr.h"
#include "udp.h"
#include "udp_queue_types.h"
#include "utils.h"

static __always_inline void block1(int *sum, int t)
{
  *sum ^= t + 0x9e37;
  *sum += 13;
  *sum *= 3;
  *sum ^= *sum >> 7;
  *sum ^= t + 0x9e37;
  *sum += 13;
  *sum *= 3;
  *sum ^= *sum >> 7;
}

static __always_inline void block2(int *sum, int t)
{
  block1(sum, t);
  block1(sum, t);
}

static __always_inline void block4(int *sum, int t)
{
  block2(sum, t);
  block2(sum, t);
}

static __always_inline void block8(int *sum, int t)
{
  block4(sum, t);
  block4(sum, t);
}

static __always_inline void block16(int *sum, int t)
{
  block8(sum, t);
  block8(sum, t);
}

static __always_inline void block32(int *sum, int t)
{
  block16(sum, t);
  block16(sum, t);
}

static __always_inline void block64(int *sum, int t)
{
  block32(sum, t);
  block32(sum, t);
}

static __always_inline void block128(int *sum, int t)
{
  block64(sum, t);
  block64(sum, t);
}

static __always_inline void block256(int *sum, int t)
{
  block128(sum, t);
  block128(sum, t);
}

static __always_inline void block512(int *sum, int t)
{
  block256(sum, t);
  block256(sum, t);
}

static __always_inline void block1024(int *sum, int t)
{
  block512(sum, t);
  block512(sum, t);
}

static __always_inline void block2048(int *sum, int t)
{
  block1024(sum, t);
  block1024(sum, t);
}

static __always_inline void block4096(int *sum, int t)
{
  block2048(sum, t);
  block2048(sum, t);
}

#ifdef CHAM_NATIVE_FAST
#define sched_head cham_native_sched_head
#endif

#define PORT_MAP 0
#define SOCK_MAP 1
#define REUPORT_MAP 2
#define CFG_MAP 2
#define UDP_MAX_PAYLOAD (FAST_L3_PKT_ROOM - sizeof(struct udp_pkt_inner))

static __always_inline struct udp_sock * udp_sock_find(struct cham_ebpf_ctx *ctx,
    __u16 local_port);
static __always_inline __u16 find_free_port(struct cham_ebpf_ctx *ctx);
static __always_inline int handle_bump_rx(struct cham_ebpf_ctx *ctx);
static __always_inline int handle_bump_tx(struct cham_ebpf_ctx *ctx);

#ifndef CHAM_NATIVE_FAST
/* Add these functions as helpers */
static void * (*ebpf_queue_tail)(struct equeue *q, __u64 elsize) = (void *) 1001;
static int (*queue_enqueue)(struct equeue *q, __u8 type) = (void *) 1002;

static void * (*ebpf_memcpy)(void *dst, void *src, size_t len) = (void *) 1003;
static void (*ebpf_print)(int a) = (void *) 1004;

static __u16 (*ebpf_ipv4_checksum)(void *ip_hdr) = (void *) 1005;
static __u16 (*ebpf_ipv4_udptcp_cksum)(void *ip_hdr, void *udp_hdr) = (void *) 1006;

static struct cham_sched_entry * (*sched_head)(struct cham_scheduler *sched,
    __u64 elsize) = (void *) 1007;
static int (*sched_pop)(struct cham_scheduler *sched) = (void *) 1008;
static int (*sched_add)(struct cham_scheduler *sched, __u32 id, __u64 priority,
    __u32 avail) = (void *) 1009;

static void * (*ebpf_map_get)(void *map_base, __u32 len) = (void *) 1010;
static void * (*ebpf_map_lookup)(void *map_base, __u64 id, __u64 elsize) = (void *) 1011;
#endif

SEC("chamelio/event_rx")
int event_rx(struct cham_ebpf_ctx *ctx)
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

  rx_base = ebpf_map_get(ctx->shm_base + sock->rx_off, sock->rx_len);
  free_bytes = sock->rx_len - sock->rx_avail;
  if (payload_len > free_bytes)
    return -1;

  tail = sock->rx_head + sock->rx_avail;
  if (tail >= sock->rx_len)
    tail -= sock->rx_len;

  if (tail + payload_len <= sock->rx_len)
  {
    ebpf_memcpy(rx_base + tail, payload, payload_len);
  }
  else
  {
    part = sock->rx_len - tail;
    ebpf_memcpy(rx_base + tail, payload, part);
    ebpf_memcpy(rx_base, (__u8 *) payload + part, payload_len - part);
  }

  /* Update number of available bytes */
  sock->rx_avail += payload_len;

  /* Send bump to application */
  q = &ctx->equeues[sock->app_bump_qid].eq;
  qe = ebpf_queue_tail(q, sizeof(struct udp_queue_bump_entry));
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

SEC("chamelio/event_tx")
int event_tx(struct cham_ebpf_ctx *ctx)
{
  return -1;
}

SEC("chamelio/event_deq")
int event_deq(struct cham_ebpf_ctx *ctx)
{
  int ret;
  int sum;
  int t;

  if ((__u8 *) ctx->qe + sizeof(*ctx->qe) > (__u8 *) ctx->shm_end)
    return -1;

  sum = 0;
  t = ctx->qe->type;
  block1024(&sum, t);
  
  /* Add this so compiler doesn't optimise dead code out */
  if (sum == 0x13579bdf)
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

static __always_inline int handle_bump_tx(struct cham_ebpf_ctx *ctx)
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
  sock = ebpf_map_lookup(map->addr,
      bump_cham->sock_id, sizeof(struct udp_sock));
  if (sock == NULL)
    return -1;

  /* Set local port for socket if it is not defined yet */
  if (sock->local_port == 0)
  {
    local_port = find_free_port(ctx);
    if (local_port == 0)
      return -1;
    map = &ctx->maps[PORT_MAP];
    port = ebpf_map_lookup(map->addr, local_port, sizeof(struct udp_port));
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
    ebpf_memcpy(payload, ctx->shm_base + sock->tx_off + sock->tx_head, payload_len);
  }
  else
  {
    part = sock->tx_len - sock->tx_head;
    ebpf_memcpy(payload, ctx->shm_base + sock->tx_off + sock->tx_head, part);
    ebpf_memcpy(payload + part, ctx->shm_base + sock->tx_off, payload_len - part);
  }

  /* Update socket */
  new_head = sock->tx_head + payload_len;
  if (new_head >= sock->tx_len)
    new_head -= sock->tx_len;
  sock->tx_head = new_head;
  sock->tx_avail -= payload_len;

  /* Send a bump to application */
  q = &ctx->equeues[sock->app_bump_qid].eq;
  qe = (struct udp_queue_bump_entry *) ebpf_queue_tail(q,
      sizeof(struct udp_queue_bump_entry));
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

static __always_inline int handle_bump_rx(struct cham_ebpf_ctx *ctx)
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
  sock = ebpf_map_lookup(map->addr, bump->sock_id, sizeof(struct udp_sock));
  if (sock == NULL)
    return -1;

  new_head = sock->rx_head + bump->rx_head;
  if (new_head >= sock->rx_len)
    new_head -= sock->rx_len;
  sock->rx_head = new_head;
  sock->rx_avail -= bump->rx_head;

  return 0;
}

static __always_inline __u16 find_free_port(struct cham_ebpf_ctx *ctx)
{
  __u16 i, nr, next_port;
  struct udp_cfg *cfg;
  struct udp_port *port;
  struct cham_map *map;

  map = &ctx->maps[CFG_MAP];
  cfg = ebpf_map_lookup(map->addr, 0, sizeof(struct udp_cfg));
  if (cfg == NULL)
    return 0;

  next_port = cfg->next_port;
  if (next_port < MIN_PORT || next_port > MAX_PORT)
    next_port = MIN_PORT;

#ifndef CHAM_NATIVE_FAST
  /* Keep the verifier from over-optimizing the backing map access. */
  (void) ebpf_map_get(map->addr, map->size);
#endif

  map = &ctx->maps[PORT_MAP];
  for (i = 0; i < UDP_PORT_SCAN_MAX; i++)
  {
    nr = next_port + i;
    if (nr > MAX_PORT)
      nr = MIN_PORT + nr - MAX_PORT - 1;

    port = ebpf_map_lookup(map->addr, nr, sizeof(struct udp_port));
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

static __always_inline struct udp_sock *udp_sock_find(struct cham_ebpf_ctx *ctx,
    __u16 local_port)
{
  struct udp_port *port;
  __u16 sock_id;
  struct cham_map *map;

  if (local_port < MIN_PORT || local_port > 65535)
    return NULL;

  map = &ctx->maps[PORT_MAP];
  port = ebpf_map_lookup(map->addr, local_port, sizeof(struct udp_port));
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
  return ebpf_map_lookup(map->addr, sock_id, sizeof(struct udp_sock));
}

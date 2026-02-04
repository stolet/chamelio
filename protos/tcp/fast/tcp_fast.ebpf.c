#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

#include "cham_fast.h"
#include "tcp_hdr.h"
#include "ip_hdr.h"
#include "tcp.h"
#include "tcp_queue_types.h"
#include "utils.h"

#define PORT_MAP 0
#define SOCK_MAP 1
#define FLOW_MAP 2
#define CFG_MAP 3

static __always_inline struct tcp_sock *tcp_flow_find(struct cham_ebpf_ctx *ctx,
    __u32 local_ip, __u16 local_port, __u32 remote_ip, __u16 remote_port);
static __always_inline int punt_ctrl_rx(struct cham_ebpf_ctx *ctx,
    struct ip_hdr *ip, struct tcp_hdr *tcp);
static __always_inline int handle_bump_rx(struct cham_ebpf_ctx *ctx);
static __always_inline int handle_bump_tx(struct cham_ebpf_ctx *ctx);
static __always_inline int handle_ctrl_tx(struct cham_ebpf_ctx *ctx);
static __always_inline void fill_headers(struct tcp_pkt_inner *p,
    __u32 local_ip, __u16 local_port, __u32 remote_ip, __u16 remote_port,
    __u32 seq, __u32 ack, __u16 flags, __u16 payload_len);
static __always_inline __u32 flow_hash(__u32 local_ip, __u16 local_port,
    __u32 remote_ip, __u16 remote_port);

/* Add these functions as helpers */
static void * (*ebpf_queue_tail)(struct equeue *q, __u64 elsize) = (void *) 1001;
static int (*queue_enqueue)(struct equeue *q, __u8 type) = (void *) 1002;
static void * (*ebpf_queue_head)(struct dqueue *q, __u64 elsize) = (void *) 1012;
static int (*queue_dequeue)(struct dqueue *q) = (void *) 1013;

static void * (*ebpf_memcpy)(void *dst, void *src, size_t len) = (void *) 1003;
static void (*ebpf_print)(int a) = (void *) 1004;

static __u16 (*ebpf_ipv4_checksum)(void *ip_hdr) = (void *) 1005;
static __u16 (*ebpf_ipv4_udptcp_cksum)(void *ip_hdr, void *udp_hdr) = (void *) 1006;

static void * (*ebpf_map_get)(void *map_base, __u32 len) = (void *) 1010;
static void * (*ebpf_map_lookup)(void *map_base, __u64 id, __u64 elsize) = (void *) 1011;

SEC("chamelio/event_rx")
int tcp_fast_event_rx(struct cham_ebpf_ctx *ctx)
{
  int ret;
  __u16 ip_hdrs_len, ip_total_len, tcp_hdrs_len, payload_len, flags;
  struct ip_hdr *ip;
  struct tcp_hdr *tcp;
  void *payload;
  struct tcp_sock *sock;
  struct equeue *q;
  struct tcp_queue_bump_entry *qe;
  struct tcp_queue_bump_app_rx *bump;
  __u8 *rx_base;
  __u32 free_bytes;
  __u32 tail;
  __u32 part;

  if (ctx->pkt + sizeof(struct ip_hdr) > ctx->pkt_end)
    return -1;

  ip = (struct ip_hdr *) ctx->pkt;
  if (IPH_V(ip) != 4 || IPH_HL(ip) < 5)
    return -1;

  if (ip->proto != IP_PROTO_TCP)
    return -1;

  if (f_beui16(ip->offset) & 0x3FFF)
    return -1;

  ip_hdrs_len = (__u16) (IPH_HL(ip) * 4);
  ip_total_len = f_beui16(ip->len);
  if (ip_total_len < ip_hdrs_len + (__u16) sizeof(struct tcp_hdr))
    return -1;

  if ((__u8 *) ip + ip_hdrs_len + sizeof(struct tcp_hdr) >
      (__u8 *) ctx->pkt_end)
    return -1;

  tcp = (struct tcp_hdr *) ((__u8 *) ip + ip_hdrs_len);
  tcp_hdrs_len = (__u16) (TCPH_HDRLEN(tcp) * 4);
  if (tcp_hdrs_len < sizeof(struct tcp_hdr))
    return -1;

  if (ip_total_len < ip_hdrs_len + tcp_hdrs_len)
    return -1;

  flags = TCPH_FLAGS(tcp);
  payload_len = ip_total_len - ip_hdrs_len - tcp_hdrs_len;

  sock = tcp_flow_find(ctx, f_beui32(ip->dst), f_beui16(tcp->dest),
      f_beui32(ip->src), f_beui16(tcp->src));
  if (sock != NULL)
  {
    if (sock->state != TCP_SOCK_STATE_ESTABLISHED || sock->opaque == 0 ||
        (flags & (TAS_TCP_SYN | TAS_TCP_FIN | TAS_TCP_RST)) != 0 ||
        (flags & ~(TAS_TCP_ACK | TAS_TCP_PSH)) != 0 || payload_len == 0)
    {
      return punt_ctrl_rx(ctx, ip, tcp);
    }

    payload = (void *) ((__u8 *) tcp + tcp_hdrs_len);
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

    sock->rx_avail += payload_len;

    q = &ctx->equeues[sock->app_bump_qid].eq;
    qe = ebpf_queue_tail(q, sizeof(struct tcp_queue_bump_entry));
    if (qe == NULL)
      return -1;

    bump = &qe->data.bump_app_rx;
    bump->opaque = sock->opaque;
    bump->rx_avail = payload_len;
    bump->rx_port = f_beui16(tcp->src);
    bump->rx_ip = f_beui32(ip->src);

    ret = queue_enqueue(q, TCP_QUEUE_BUMP_APP_RX);
    if (ret != 0)
      return -1;

    return 0;
  }

  if (flags != TAS_TCP_SYN || payload_len != 0)
    return -1;

  return punt_ctrl_rx(ctx, ip, tcp);
}

SEC("chamelio/event_tx")
int tcp_fast_event_tx(struct cham_ebpf_ctx *ctx)
{
  return -1;
}

SEC("chamelio/event_deq")
int tcp_fast_event_deq(struct cham_ebpf_ctx *ctx)
{
  if ((__u8 *) ctx->qe + sizeof(struct tcp_queue_bump_entry) >
      (__u8 *) ctx->shm_end)
    return -1;

  switch (ctx->qe->type)
  {
    case TCP_QUEUE_BUMP_CHAM_TX:
      return handle_bump_tx(ctx);
    case TCP_QUEUE_BUMP_CHAM_RX:
      return handle_bump_rx(ctx);
    case TCP_QUEUE_CTRL_TX:
      return handle_ctrl_tx(ctx);
    default:
      return -1;
  }
}

static __always_inline int handle_bump_tx(struct cham_ebpf_ctx *ctx)
{
  int ret;
  void *payload;
  struct tcp_sock *sock;
  struct equeue *q;
  struct tcp_queue_bump_entry *qe;
  struct tcp_queue_bump_cham_tx *bump_cham;
  struct tcp_queue_bump_app_tx *bump_app;
  struct cham_map *map;
  struct tcp_pkt_inner *p = (struct tcp_pkt_inner *) ctx->pkt;
  __u16 payload_len, pkt_hdrs_len;
  __u32 new_head;
  __u32 tx_ip;
  __u16 tx_port;
  __u64 part;

  if ((__u8 *) p + sizeof(struct tcp_pkt_inner) > (__u8 *) ctx->pkt_end)
    return -1;

  qe = (struct tcp_queue_bump_entry *) ctx->qe;
  bump_cham = &qe->data.bump_cham_tx;

  map = &ctx->maps[SOCK_MAP];
  sock = ebpf_map_lookup(map->addr, bump_cham->sock_id, sizeof(struct tcp_sock));
  if (sock == NULL || sock->state != TCP_SOCK_STATE_ESTABLISHED)
    return -1;

  payload_len = bump_cham->tx_avail;
  if (payload_len > TCP_MSS)
    payload_len = TCP_MSS;

  if ((__u8 *) p + sizeof(struct tcp_pkt_inner) + payload_len >
      (__u8 *) ctx->pkt_end)
    return -1;

  tx_ip = bump_cham->tx_ip == 0 ? sock->remote_ip : bump_cham->tx_ip;
  tx_port = bump_cham->tx_port == 0 ? sock->remote_port : bump_cham->tx_port;
  if (tx_port == 0 || tx_ip == 0)
    return -1;

  sock->remote_ip = tx_ip;
  sock->remote_port = tx_port;

  fill_headers(p, sock->local_ip, sock->local_port, tx_ip, tx_port,
      sock->tx_seq, sock->rx_seq, TAS_TCP_ACK | TAS_TCP_PSH, payload_len);
  pkt_hdrs_len = sizeof(struct ip_hdr) + TCP_HLEN;

  payload = ctx->pkt + pkt_hdrs_len;
  if (sock->tx_head + payload_len <= sock->tx_len)
  {
    ebpf_memcpy(payload, ctx->shm_base + sock->tx_off + sock->tx_head,
        payload_len);
  }
  else
  {
    part = sock->tx_len - sock->tx_head;
    ebpf_memcpy(payload, ctx->shm_base + sock->tx_off + sock->tx_head, part);
    ebpf_memcpy(payload + part, ctx->shm_base + sock->tx_off,
        payload_len - part);
  }

  new_head = sock->tx_head + payload_len;
  if (new_head >= sock->tx_len)
    new_head -= sock->tx_len;
  sock->tx_head = new_head;
  sock->tx_avail -= payload_len;
  sock->tx_seq += payload_len;

  p->ip.chksum = 0;
  p->tcp.chksum = 0;
  p->ip.chksum = ebpf_ipv4_checksum(&p->ip);
  p->tcp.chksum = ebpf_ipv4_udptcp_cksum(&p->ip, &p->tcp);

  q = &ctx->equeues[sock->app_bump_qid].eq;
  qe = (struct tcp_queue_bump_entry *) ebpf_queue_tail(q,
      sizeof(struct tcp_queue_bump_entry));
  if (qe == NULL)
    return -1;

  bump_app = &qe->data.bump_app_tx;
  bump_app->opaque = sock->opaque;
  bump_app->tx_head = payload_len;

  ret = queue_enqueue(q, TCP_QUEUE_BUMP_APP_TX);
  if (ret != 0)
    return -1;

  return pkt_hdrs_len + payload_len;
}

static __always_inline int handle_bump_rx(struct cham_ebpf_ctx *ctx)
{
  __u32 new_head;
  struct tcp_sock *sock;
  struct tcp_queue_bump_cham_rx *bump;
  struct tcp_queue_bump_entry *qe;
  struct cham_map *map;

  qe = (struct tcp_queue_bump_entry *) ctx->qe;
  bump = &qe->data.bump_cham_rx;

  map = &ctx->maps[SOCK_MAP];
  sock = ebpf_map_lookup(map->addr, bump->sock_id, sizeof(struct tcp_sock));
  if (sock == NULL)
    return -1;

  new_head = sock->rx_head + bump->rx_head;
  if (new_head >= sock->rx_len)
    new_head -= sock->rx_len;
  sock->rx_head = new_head;
  sock->rx_avail -= bump->rx_head;

  return 0;
}

static __always_inline int handle_ctrl_tx(struct cham_ebpf_ctx *ctx)
{
  int ret;
  __u16 pkt_len;
  struct cham_map *map;
  struct tcp_ctrl_cfg *cfg;
  struct dqueue *pkt_q;
  struct tcp_queue_bump_entry *pkt_qe;
  struct tcp_pkt_inner *pkt;
  struct tcp_queue_bump_entry *qe;
  struct tcp_pkt_inner *p = (struct tcp_pkt_inner *) ctx->pkt;

  if ((__u8 *) p + sizeof(struct tcp_pkt_inner) > (__u8 *) ctx->pkt_end)
    return -1;

  qe = (struct tcp_queue_bump_entry *) ctx->qe;
  if (qe->type != TCP_QUEUE_CTRL_TX)
    return -1;

  map = &ctx->maps[CFG_MAP];
  cfg = ebpf_map_lookup(map->addr, 0, sizeof(struct tcp_ctrl_cfg));
  if (cfg == NULL || ctx->dqueues == NULL)
    return -1;

  pkt_q = &ctx->dqueues[cfg->slow_fast_pkt_qid].dq;
  pkt_qe = ebpf_queue_head(pkt_q, sizeof(struct tcp_queue_bump_entry));
  if (pkt_qe == NULL)
    return -1;

  if ((__u8 *) pkt_qe + sizeof(struct tcp_queue_bump_entry) >
      (__u8 *) ctx->shm_end)
  {
    return -1;
  }
  if (pkt_qe->type != TCP_QUEUE_CTRL_TX_PKT)
    return -1;

  pkt = &pkt_qe->data.ctrl_pkt.pkt;
  pkt_len = f_beui16(pkt->ip.len);
  if (pkt_len < sizeof(struct tcp_pkt_inner))
    return -1;
  if ((__u8 *) p + pkt_len > (__u8 *) ctx->pkt_end)
    return -1;

  ebpf_memcpy(p, pkt, sizeof(*pkt));
  p->ip.chksum = 0;
  p->tcp.chksum = 0;
  p->ip.chksum = ebpf_ipv4_checksum(&p->ip);
  p->tcp.chksum = ebpf_ipv4_udptcp_cksum(&p->ip, &p->tcp);

  ret = queue_dequeue(pkt_q);
  if (ret != 0)
    return -1;

  return pkt_len;
}

static __always_inline struct tcp_sock *tcp_flow_find(struct cham_ebpf_ctx *ctx,
    __u32 local_ip, __u16 local_port, __u32 remote_ip, __u16 remote_port)
{
  int i;
  __u32 sid;
  __u32 hash;
  struct tcp_flow_bucket *bucket;
  struct tcp_sock *sock;
  struct cham_map *map;

  hash = flow_hash(local_ip, local_port, remote_ip, remote_port)
      % TCP_FLOW_BUCKETS;

  map = &ctx->maps[FLOW_MAP];
  bucket = ebpf_map_lookup(map->addr, hash, sizeof(struct tcp_flow_bucket));
  if (bucket == NULL)
    return NULL;

  map = &ctx->maps[SOCK_MAP];
  for (i = 0; i < TCP_FLOW_BUCKET_SLOTS; i++)
  {
    sid = bucket->sids[i];
    if (sid == ID_INVALID)
      continue;
    sock = ebpf_map_lookup(map->addr, sid, sizeof(struct tcp_sock));
    if (sock == NULL)
      continue;
    if (sock->state == TCP_SOCK_STATE_CLOSED)
      continue;
    if (sock->local_ip == local_ip && sock->local_port == local_port &&
        sock->remote_ip == remote_ip && sock->remote_port == remote_port)
    {
      return sock;
    }
  }

  return NULL;
}

static __always_inline int punt_ctrl_rx(struct cham_ebpf_ctx *ctx,
    struct ip_hdr *ip, struct tcp_hdr *tcp)
{
  int ret;
  struct equeue *sig_q;
  struct equeue *pkt_q;
  struct tcp_ctrl_cfg *cfg;
  struct tcp_queue_bump_entry *sig_qe;
  struct tcp_queue_bump_entry *pkt_qe;
  struct cham_map *map;

  map = &ctx->maps[CFG_MAP];
  cfg = ebpf_map_lookup(map->addr, 0, sizeof(struct tcp_ctrl_cfg));
  if (cfg == NULL)
    return -1;

  pkt_q = &ctx->equeues[cfg->fast_slow_pkt_qid].eq;
  pkt_qe = ebpf_queue_tail(pkt_q, sizeof(struct tcp_queue_bump_entry));
  if (pkt_qe == NULL)
    return -1;

  sig_q = &ctx->equeues[cfg->fast_slow_sig_qid].eq;
  sig_qe = ebpf_queue_tail(sig_q, sizeof(struct tcp_queue_bump_entry));
  if (sig_qe == NULL)
    return -1;

  ebpf_memcpy(&pkt_qe->data.ctrl_pkt.pkt.ip, ip, sizeof(*ip));
  ebpf_memcpy(&pkt_qe->data.ctrl_pkt.pkt.tcp, tcp, sizeof(*tcp));
  sig_qe->data.ctrl_sig.ready = 1;

  ret = queue_enqueue(pkt_q, TCP_QUEUE_CTRL_RX_PKT);
  if (ret != 0)
    return -1;

  ret = queue_enqueue(sig_q, TCP_QUEUE_CTRL_RX);
  if (ret != 0)
    return -1;

  return 0;
}

static __always_inline void fill_headers(struct tcp_pkt_inner *p,
    __u32 local_ip, __u16 local_port, __u32 remote_ip, __u16 remote_port,
    __u32 seq, __u32 ack, __u16 flags, __u16 payload_len)
{
  IPH_VHL_SET(&p->ip, 4, 5);
  p->ip._tos = 0;
  p->ip.len = t_beui16(sizeof(struct ip_hdr) + TCP_HLEN + payload_len);
  p->ip.id = t_beui16(3);
  p->ip.offset = t_beui16(0);
  p->ip.ttl = 0xff;
  p->ip.proto = IP_PROTO_TCP;
  p->ip.src = t_beui32(local_ip);
  p->ip.dst = t_beui32(remote_ip);
  p->ip.chksum = 0;

  p->tcp.src = t_beui16(local_port);
  p->tcp.dest = t_beui16(remote_port);
  p->tcp.seqno = t_beui32(seq);
  p->tcp.ackno = t_beui32(ack);
  TCPH_HDRLEN_FLAGS_SET(&p->tcp, TCP_HLEN / 4, flags);
  p->tcp.wnd = t_beui16(65535);
  p->tcp.chksum = 0;
  p->tcp.urgp = t_beui16(0);
}

static __always_inline __u32 flow_hash(__u32 local_ip, __u16 local_port,
    __u32 remote_ip, __u16 remote_port)
{
  return local_ip ^ remote_ip ^ ((__u32) local_port << 16) ^ remote_port;
}

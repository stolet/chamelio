#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "tcp_slow_internal.h"
#include "queue_fns.h"
#include "tcp_hdr.h"
#include "log.h"

static inline __u32 flow_hash(__u32 local_ip, __u16 local_port, __u32 remote_ip,
    __u16 remote_port);
static int enqueue_ctrl_pkt(struct tcp_slow_context *ctx, __u32 local_ip,
    __u16 local_port, __u32 remote_ip, __u16 remote_port, __u32 seq,
    __u32 ack, __u16 flags);
static void fill_ctrl_pkt(struct tcp_pkt_inner *pkt, __u32 local_ip,
    __u16 local_port, __u32 remote_ip, __u16 remote_port, __u32 seq,
    __u32 ack, __u16 flags);

int tcp_slow_state_alloc_sock(struct tcp_slow_context *ctx, __u64 opaque,
    __u8 app_id, __u8 ctx_id, __u16 app_bump_qid, struct tcp_sock **sock_out)
{
  struct tcp_sock *sock;
  struct proto_queue_lib *protoq;

  if (ctx->n_socks >= MAX_SOCKETS)
  {
    LOG_ERROR("socket map is full");
    return -1;
  }
  sock = &tcp_slow_get_sock_map(ctx)[ctx->n_socks];

  memset(sock, 0, sizeof(*sock));
  sock->id = ctx->n_socks;
  sock->opaque = opaque;
  sock->core = 0;
  sock->app_bump_qid = app_bump_qid;
  sock->local_ip = ctx->proto->local_ip;
  sock->state = TCP_SOCK_STATE_INIT;
  sock->app_id = app_id;
  sock->ctx_id = ctx_id;

  protoq = cham_new_queue(ctx->proto, RXBUF_SZ, 1);
  if (protoq == NULL)
  {
    LOG_ERROR("failed to create RX queue for socket");
    return -1;
  }
  sock->rx_len = protoq->elsize * protoq->nelems;
  sock->rx_avail = 0;
  sock->rx_head = 0;
  sock->rx_off = protoq->off;

  protoq = cham_new_queue(ctx->proto, TXBUF_SZ, 1);
  if (protoq == NULL)
  {
    LOG_ERROR("failed to create TX queue for socket");
    return -1;
  }
  sock->tx_len = protoq->elsize * protoq->nelems;
  sock->tx_avail = 0;
  sock->tx_head = 0;
  sock->tx_off = protoq->off;

  ctx->sock_meta[sock->id].listener_id = ID_INVALID;
  ctx->sock_meta[sock->id].auto_bound = 0;
  ctx->n_socks++;

  *sock_out = sock;
  return 0;
}

int tcp_slow_state_fill_new_sock_res(struct tcp_sock *sock,
    struct tcp_queue_new_sock_res *res)
{
  res->opaque = sock->opaque;
  res->sock_id = sock->id;
  res->core = sock->core;
  res->rx_qid = 0;
  res->rx_len = sock->rx_len;
  res->rx_off = sock->rx_off;
  res->tx_qid = 0;
  res->tx_len = sock->tx_len;
  res->tx_off = sock->tx_off;

  return 0;
}

int tcp_slow_state_fill_accept_res(struct tcp_sock *listen_sock,
    struct tcp_sock *sock, struct tcp_queue_accept_res *res)
{
  res->status = 0;
  res->opaque = sock->opaque;
  res->listen_opaque = listen_sock->opaque;
  res->sock_id = sock->id;
  res->core = sock->core;
  res->rx_qid = 0;
  res->rx_len = sock->rx_len;
  res->rx_off = sock->rx_off;
  res->tx_qid = 0;
  res->tx_len = sock->tx_len;
  res->tx_off = sock->tx_off;
  res->local_ip = sock->local_ip;
  res->local_port = sock->local_port;
  res->remote_ip = sock->remote_ip;
  res->remote_port = sock->remote_port;

  return 0;
}

int tcp_slow_state_bound_insert(struct tcp_slow_context *ctx, __u16 port,
    __u32 sock_id, int reuseport)
{
  int i;
  struct tcp_port *bp;
  struct tcp_sock *socks;

  bp = &ctx->bound_ports[port];
  socks = tcp_slow_get_sock_map(ctx);

  if (bp->nsocks == 0)
  {
    bp->sids[0] = sock_id;
    bp->nsocks = 1;
    bp->next_sock = 0;
    return 0;
  }

  if (!reuseport || bp->nsocks >= MAX_REUSOCK_PORT)
    return -EADDRINUSE;

  for (i = 0; i < bp->nsocks; i++)
  {
    if (!socks[bp->sids[i]].reuport)
      return -EADDRINUSE;
  }

  bp->sids[bp->nsocks] = sock_id;
  bp->nsocks++;
  return 0;
}

void tcp_slow_state_bound_remove(struct tcp_slow_context *ctx, __u16 port,
    __u32 sock_id)
{
  int i;
  struct tcp_port *bp;

  if (port == 0 || port > MAX_PORT)
    return;

  bp = &ctx->bound_ports[port];
  for (i = 0; i < bp->nsocks; i++)
  {
    if (bp->sids[i] != sock_id)
      continue;

    for (; i + 1 < bp->nsocks; i++)
      bp->sids[i] = bp->sids[i + 1];
    bp->nsocks--;
    bp->sids[bp->nsocks] = ID_INVALID;
    if (bp->next_sock >= bp->nsocks)
      bp->next_sock = 0;
    return;
  }
}

__u16 tcp_slow_state_bound_find_free_port(struct tcp_slow_context *ctx)
{
  __u16 port;

  for (port = MIN_PORT; port <= MAX_PORT; port++)
  {
    if (ctx->bound_ports[port].nsocks == 0)
      return port;
  }

  return 0;
}

int tcp_slow_state_listener_insert(struct tcp_slow_context *ctx, __u16 port,
    __u32 sock_id, int reuseport)
{
  int i;
  struct tcp_port *ports;
  struct tcp_port *port_entry;
  struct tcp_sock *socks;

  ports = tcp_slow_get_listener_map(ctx);
  port_entry = &ports[port];
  socks = tcp_slow_get_sock_map(ctx);

  if (port_entry->nsocks == 0)
  {
    port_entry->sids[0] = sock_id;
    port_entry->nsocks = 1;
    port_entry->next_sock = 0;
    return 0;
  }

  if (!reuseport || port_entry->nsocks >= MAX_REUSOCK_PORT)
    return -EADDRINUSE;

  for (i = 0; i < port_entry->nsocks; i++)
  {
    if (!socks[port_entry->sids[i]].reuport)
      return -EADDRINUSE;
  }

  port_entry->sids[port_entry->nsocks] = sock_id;
  port_entry->nsocks++;
  return 0;
}

void tcp_slow_state_listener_remove(struct tcp_slow_context *ctx, __u16 port,
    __u32 sock_id)
{
  int i;
  struct tcp_port *ports;
  struct tcp_port *port_entry;

  if (port == 0 || port > MAX_PORT)
    return;

  ports = tcp_slow_get_listener_map(ctx);
  port_entry = &ports[port];
  for (i = 0; i < port_entry->nsocks; i++)
  {
    if (port_entry->sids[i] != sock_id)
      continue;

    for (; i + 1 < port_entry->nsocks; i++)
      port_entry->sids[i] = port_entry->sids[i + 1];
    port_entry->nsocks--;
    port_entry->sids[port_entry->nsocks] = ID_INVALID;
    if (port_entry->next_sock >= port_entry->nsocks)
      port_entry->next_sock = 0;
    return;
  }
}

__u32 tcp_slow_state_listener_lookup(struct tcp_slow_context *ctx,
    __u32 local_ip, __u16 port)
{
  struct tcp_port *ports;
  struct tcp_port *port_entry;
  struct tcp_sock *socks;
  __u32 sid;
  int i, idx;

  if (port == 0 || port > MAX_PORT)
    return ID_INVALID;

  ports = tcp_slow_get_listener_map(ctx);
  port_entry = &ports[port];
  socks = tcp_slow_get_sock_map(ctx);

  if (port_entry->nsocks == 0)
    return ID_INVALID;

  for (i = 0; i < port_entry->nsocks; i++)
  {
    idx = (port_entry->next_sock + i) % port_entry->nsocks;
    sid = port_entry->sids[idx];
    if (sid == ID_INVALID)
      continue;
    if (socks[sid].state != TCP_SOCK_STATE_LISTEN)
      continue;
    if (socks[sid].local_ip != 0 && socks[sid].local_ip != local_ip)
      continue;
    port_entry->next_sock = (idx + 1) % port_entry->nsocks;
    return sid;
  }

  return ID_INVALID;
}

void tcp_slow_state_listener_reset(struct tcp_slow_context *ctx,
    __u32 listener_id)
{
  struct tcp_listener_slow *listener;

  listener = &ctx->listeners[listener_id];
  free(listener->ready_sids);
  memset(listener, 0, sizeof(*listener));
}

void tcp_slow_state_listener_detach_child(struct tcp_slow_context *ctx,
    struct tcp_sock *sock)
{
  __u32 i;
  __u32 listener_id;
  struct tcp_listener_slow *listener;

  listener_id = ctx->sock_meta[sock->id].listener_id;
  if (listener_id == ID_INVALID)
    return;

  listener = &ctx->listeners[listener_id];
  if (listener->active && listener->ready_sids != NULL)
  {
    for (i = 0; i < listener->backlog_len; i++)
    {
      if (listener->ready_sids[i] != sock->id)
        continue;

      listener->ready_sids[i] = ID_INVALID;
      break;
    }
  }

  if (listener->backlog_used > 0)
    listener->backlog_used--;
  ctx->sock_meta[sock->id].listener_id = ID_INVALID;
}

int tcp_slow_state_listener_ready_push(struct tcp_slow_context *ctx,
    __u32 listener_id, __u32 sock_id)
{
  __u32 pos;
  struct tcp_listener_slow *listener;

  listener = &ctx->listeners[listener_id];
  if (!listener->active || listener->ready_used >= listener->backlog_len)
    return -1;

  pos = listener->ready_head + listener->ready_used;
  if (pos >= listener->backlog_len)
    pos -= listener->backlog_len;
  listener->ready_sids[pos] = sock_id;
  listener->ready_used++;

  return 0;
}

struct tcp_sock *tcp_slow_state_listener_ready_pop(
    struct tcp_slow_context *ctx, __u32 listener_id)
{
  __u32 sid;
  struct tcp_listener_slow *listener;
  struct tcp_sock *sock;

  listener = &ctx->listeners[listener_id];
  while (listener->active && listener->ready_used != 0)
  {
    sid = listener->ready_sids[listener->ready_head];
    listener->ready_sids[listener->ready_head] = ID_INVALID;
    listener->ready_head++;
    if (listener->ready_head >= listener->backlog_len)
      listener->ready_head = 0;
    listener->ready_used--;

    if (sid == ID_INVALID)
      continue;

    sock = &tcp_slow_get_sock_map(ctx)[sid];
    if (ctx->sock_meta[sock->id].listener_id != listener_id)
      continue;
    if (sock->state != TCP_SOCK_STATE_ACCEPT_PENDING)
    {
      ctx->sock_meta[sock->id].listener_id = ID_INVALID;
      if (listener->backlog_used > 0)
        listener->backlog_used--;
      continue;
    }

    return sock;
  }

  return NULL;
}

void tcp_slow_state_listener_abort_children(struct tcp_slow_context *ctx,
    struct tcp_sock *listen_sock)
{
  __u32 i;
  __u32 listener_id;
  struct tcp_sock *sock;

  listener_id = listen_sock->id;
  for (i = 0; i < ctx->n_socks; i++)
  {
    if (ctx->sock_meta[i].listener_id != listener_id)
      continue;

    sock = &tcp_slow_get_sock_map(ctx)[i];
    if (sock->state == TCP_SOCK_STATE_CLOSED)
      continue;

    if (sock->remote_port != 0)
      tcp_slow_state_enqueue_ctrl_tx(ctx, sock, TAS_TCP_RST | TAS_TCP_ACK);
    tcp_slow_state_sock_close_final(ctx, sock);
  }
}

int tcp_slow_state_flow_insert(struct tcp_slow_context *ctx,
    struct tcp_sock *sock)
{
  int i;
  __u32 hash;
  struct tcp_flow_bucket *bucket;

  hash = flow_hash(sock->local_ip, sock->local_port,
      sock->remote_ip, sock->remote_port) % TCP_FLOW_BUCKETS;
  bucket = &tcp_slow_get_flow_map(ctx)[hash];

  for (i = 0; i < TCP_FLOW_BUCKET_SLOTS; i++)
  {
    if (bucket->sids[i] != ID_INVALID)
      continue;
    bucket->sids[i] = sock->id;
    return 0;
  }

  LOG_WARN("TCP flow bucket full for local_port=%u remote_port=%u",
      sock->local_port, sock->remote_port);
  return -1;
}

void tcp_slow_state_flow_remove(struct tcp_slow_context *ctx,
    struct tcp_sock *sock)
{
  int i;
  __u32 hash;
  struct tcp_flow_bucket *bucket;

  hash = flow_hash(sock->local_ip, sock->local_port,
      sock->remote_ip, sock->remote_port) % TCP_FLOW_BUCKETS;
  bucket = &tcp_slow_get_flow_map(ctx)[hash];

  for (i = 0; i < TCP_FLOW_BUCKET_SLOTS; i++)
  {
    if (bucket->sids[i] != sock->id)
      continue;
    bucket->sids[i] = ID_INVALID;
    return;
  }
}

struct tcp_sock *tcp_slow_state_flow_lookup(struct tcp_slow_context *ctx,
    __u32 local_ip, __u16 local_port, __u32 remote_ip, __u16 remote_port)
{
  int i;
  __u32 hash, sid;
  struct tcp_flow_bucket *bucket;
  struct tcp_sock *socks;
  struct tcp_sock *sock;

  hash = flow_hash(local_ip, local_port, remote_ip, remote_port)
      % TCP_FLOW_BUCKETS;
  bucket = &tcp_slow_get_flow_map(ctx)[hash];
  socks = tcp_slow_get_sock_map(ctx);

  for (i = 0; i < TCP_FLOW_BUCKET_SLOTS; i++)
  {
    sid = bucket->sids[i];
    if (sid == ID_INVALID)
      continue;
    sock = &socks[sid];
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

int tcp_slow_state_enqueue_ctrl_tx(struct tcp_slow_context *ctx,
    struct tcp_sock *sock, __u16 flags)
{
  int ret;

  ret = enqueue_ctrl_pkt(ctx, sock->local_ip, sock->local_port,
      sock->remote_ip, sock->remote_port, sock->tx_seq, sock->rx_seq, flags);
  if (ret != 0)
    return -1;

  if ((flags & TAS_TCP_SYN) != 0 || (flags & TAS_TCP_FIN) != 0)
    sock->tx_seq++;

  return 0;
}

int tcp_slow_state_enqueue_ctrl_reply(struct tcp_slow_context *ctx,
    __u32 local_ip, __u16 local_port, __u32 remote_ip, __u16 remote_port,
    __u32 seq, __u32 ack, __u16 flags)
{
  return enqueue_ctrl_pkt(ctx, local_ip, local_port, remote_ip, remote_port,
      seq, ack, flags);
}

void tcp_slow_state_sock_close_final(struct tcp_slow_context *ctx,
    struct tcp_sock *sock)
{
  tcp_slow_timeout_cancel(ctx, sock);
  tcp_slow_state_listener_detach_child(ctx, sock);
  tcp_slow_state_flow_remove(ctx, sock);
  if (ctx->sock_meta[sock->id].listener_id == ID_INVALID)
    tcp_slow_state_bound_remove(ctx, sock->local_port, sock->id);

  sock->state = TCP_SOCK_STATE_CLOSED;
  sock->flags = 0;
  sock->remote_ip = 0;
  sock->remote_port = 0;
  sock->tx_seq = 0;
  sock->rx_seq = 0;
  ctx->sock_meta[sock->id].auto_bound = 0;
}

void tcp_slow_state_sock_connect_failed(struct tcp_slow_context *ctx,
    struct tcp_sock *sock, int err)
{
  struct tcp_app_context_slow *actx;

  tcp_slow_timeout_cancel(ctx, sock);
  tcp_slow_state_flow_remove(ctx, sock);
  if (ctx->sock_meta[sock->id].auto_bound)
  {
    tcp_slow_state_bound_remove(ctx, sock->local_port, sock->id);
    sock->local_port = 0;
    sock->local_ip = ctx->proto->local_ip;
    ctx->sock_meta[sock->id].auto_bound = 0;
  }

  sock->remote_ip = 0;
  sock->remote_port = 0;
  sock->tx_seq = 0;
  sock->rx_seq = 0;
  sock->flags = 0;
  sock->state = TCP_SOCK_STATE_INIT;

  actx = tcp_slow_sock_actx(ctx, sock);
  tcp_slow_app_enqueue_connect_res(actx, sock->opaque, err, sock);
}

static inline __u32 flow_hash(__u32 local_ip, __u16 local_port, __u32 remote_ip,
    __u16 remote_port)
{
  return local_ip ^ remote_ip ^ ((__u32) local_port << 16) ^ remote_port;
}

static int enqueue_ctrl_pkt(struct tcp_slow_context *ctx, __u32 local_ip,
    __u16 local_port, __u32 remote_ip, __u16 remote_port, __u32 seq,
    __u32 ack, __u16 flags)
{
  int ret;
  struct tcp_queue_bump_entry *pkt_qe;
  struct tcp_queue_bump_entry *sig_qe;

  pkt_qe = queue_tail(ctx->slow_fast_pkt_q);
  if (pkt_qe == NULL)
  {
    LOG_WARN("slow->fast control packet queue is full");
    return -1;
  }

  sig_qe = queue_tail(ctx->slow_fast_sig_q);
  if (sig_qe == NULL)
  {
    LOG_WARN("slow->fast control signal queue is full");
    return -1;
  }

  fill_ctrl_pkt(&pkt_qe->data.ctrl_pkt.pkt, local_ip, local_port,
      remote_ip, remote_port, seq, ack, flags);
  sig_qe->data.ctrl_sig.ready = 1;

  ret = queue_enqueue(ctx->slow_fast_pkt_q, TCP_QUEUE_CTRL_TX_PKT);
  if (ret != 0)
  {
    LOG_WARN("failed to enqueue TCP control packet data");
    return -1;
  }

  ret = queue_enqueue(ctx->slow_fast_sig_q, TCP_QUEUE_CTRL_TX);
  if (ret != 0)
  {
    LOG_WARN("failed to enqueue TCP control packet signal");
    return -1;
  }

  return 0;
}

static void fill_ctrl_pkt(struct tcp_pkt_inner *pkt, __u32 local_ip,
    __u16 local_port, __u32 remote_ip, __u16 remote_port, __u32 seq,
    __u32 ack, __u16 flags)
{
  IPH_VHL_SET(&pkt->ip, 4, 5);
  pkt->ip._tos = 0;
  pkt->ip.len = t_beui16(sizeof(*pkt));
  pkt->ip.id = t_beui16(3);
  pkt->ip.offset = t_beui16(0);
  pkt->ip.ttl = 0xff;
  pkt->ip.proto = IP_PROTO_TCP;
  pkt->ip.src = t_beui32(local_ip);
  pkt->ip.dst = t_beui32(remote_ip);
  pkt->ip.chksum = 0;

  pkt->tcp.src = t_beui16(local_port);
  pkt->tcp.dest = t_beui16(remote_port);
  pkt->tcp.seqno = t_beui32(seq);
  pkt->tcp.ackno = t_beui32(ack);
  TCPH_HDRLEN_FLAGS_SET(&pkt->tcp, TCP_HLEN / 4, flags);
  pkt->tcp.wnd = t_beui16(65535);
  pkt->tcp.chksum = 0;
  pkt->tcp.urgp = t_beui16(0);
}

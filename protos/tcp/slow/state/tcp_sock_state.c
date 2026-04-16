#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "tcp_internal.h"
#include "clock.h"
#include "log.h"

/*** Socket Helpers ***********************************************************/

static void sock_conn_reset(struct tcp_sock *sock);
static void sock_autobind_drop(struct tcp_slow_context *ctx,
    struct tcp_sock *sock);

/*** Socket API ***************************************************************/

int tcp_sock_alloc(struct tcp_slow_context *ctx, __u64 opaque, __u8 app_id,
    __u8 ctx_id, __u16 app_bump_qid, struct tcp_sock **sock_out)
{
  struct tcp_sock *sock;
  struct tcp_sock_meta_slow *meta;
  struct proto_queue_lib *protoq;

  if (ctx->n_socks >= MAX_SOCKETS)
  {
    LOG_ERROR("socket map is full");
    return -1;
  }

  sock = &tcp_sock_map(ctx)[ctx->n_socks];
  memset(sock, 0, sizeof(*sock));
  sock->id = ctx->n_socks;
  sock->opaque = opaque;
  sock->core = 0;
  sock->app_bump_qid = app_bump_qid;
  sock->local_ip = ctx->proto->local_ip;
  sock->state = TCP_SOCK_STATE_INIT;
  sock->app_id = app_id;
  sock->ctx_id = ctx_id;
  sock->lock = 0;

  protoq = cham_new_queue(ctx->proto, ctx->config.rxbuf_sz, 1);
  if (protoq == NULL)
  {
    LOG_ERROR("failed to create RX queue for socket");
    return -1;
  }
  sock->rx_len = protoq->elsize * protoq->nelems;
  sock->rx_avail = 0;
  sock->rx_head = 0;
  sock->rx_off = protoq->off;

  protoq = cham_new_queue(ctx->proto, ctx->config.txbuf_sz, 1);
  if (protoq == NULL)
  {
    LOG_ERROR("failed to create TX queue for socket");
    return -1;
  }
  sock->tx_len = protoq->elsize * protoq->nelems;
  sock->tx_avail = 0;
  sock->tx_head = 0;
  sock->tx_pending = 0;
  sock->tx_off = protoq->off;
  sock->tx_remote_avail = 0;
  sock->tx_rexmit_seq = 0;
  sock->tx_rexmit_end_seq = 0;
  sock->rx_dupack_cnt = 0;
  sock->ts_recent = 0;
  sock->rtt_est = 0;

  meta = &ctx->sock_meta[sock->id];
  meta->listener_id = ID_INVALID;
  meta->auto_bound = 0;

  ctx->n_socks++;
  *sock_out = sock;
  return 0;
}

int tcp_sock_new_res_fill(struct tcp_sock *sock, struct tcp_queue_new_sock_res *res)
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

int tcp_sock_accept_res_fill(struct tcp_sock *listen_sock, struct tcp_sock *sock,
    struct tcp_queue_accept_res *res)
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

__u16 tcp_sock_rx_wnd(const struct tcp_sock *sock)
{
  __u32 wnd;

  wnd = sock->rx_len - sock->rx_avail;
  if (wnd > 65535)
    wnd = 65535;

  return (__u16) wnd;
}

void tcp_sock_close_final(struct tcp_slow_context *ctx, struct tcp_sock *sock)
{
  tcp_timeout_cancel(ctx, sock);
  tcp_listener_detach_child(ctx, sock);
  tcp_flow_remove(ctx, sock);
  if (tcp_sock_meta(ctx, sock)->listener_id == ID_INVALID)
    tcp_bound_remove(ctx, sock->local_port, sock->id);

  sock->state = TCP_SOCK_STATE_CLOSED;
  sock_conn_reset(sock);
  tcp_sock_meta(ctx, sock)->auto_bound = 0;
  tcp_sock_cc_reset(ctx, sock);
}

void tcp_sock_connect_fail(struct tcp_slow_context *ctx, struct tcp_sock *sock,
    int err)
{
  struct tcp_app_context_slow *actx;

  tcp_timeout_cancel(ctx, sock);
  tcp_flow_remove(ctx, sock);
  sock_autobind_drop(ctx, sock);
  sock_conn_reset(sock);
  tcp_sock_cc_reset(ctx, sock);
  sock->state = TCP_SOCK_STATE_INIT;

  actx = tcp_sock_actx(ctx, sock);
  tcp_app_connect_res(actx, sock->opaque, err, sock);
}

/*** Bound Ports **************************************************************/

int tcp_bound_insert(struct tcp_slow_context *ctx, __u16 port, __u32 sock_id,
    int reuseport)
{
  int i;
  struct tcp_port *bound;
  struct tcp_sock *socks;

  bound = &ctx->bound_ports[port];
  socks = tcp_sock_map(ctx);

  if (bound->nsocks == 0)
  {
    bound->sids[0] = sock_id;
    bound->nsocks = 1;
    bound->next_sock = 0;
    return 0;
  }

  if (!reuseport || bound->nsocks >= MAX_REUSOCK_PORT)
    return -EADDRINUSE;

  for (i = 0; i < bound->nsocks; i++)
  {
    if (!socks[bound->sids[i]].reuport)
      return -EADDRINUSE;
  }

  bound->sids[bound->nsocks] = sock_id;
  bound->nsocks++;
  return 0;
}

void tcp_bound_remove(struct tcp_slow_context *ctx, __u16 port, __u32 sock_id)
{
  int i;
  struct tcp_port *bound;

  if (port == 0 || port > MAX_PORT)
    return;

  bound = &ctx->bound_ports[port];
  for (i = 0; i < bound->nsocks; i++)
  {
    if (bound->sids[i] != sock_id)
      continue;

    for (; i + 1 < bound->nsocks; i++)
      bound->sids[i] = bound->sids[i + 1];
    bound->nsocks--;
    bound->sids[bound->nsocks] = ID_INVALID;
    if (bound->next_sock >= bound->nsocks)
      bound->next_sock = 0;
    return;
  }
}

__u16 tcp_bound_find_free(struct tcp_slow_context *ctx)
{
  __u16 port;

  for (port = MIN_PORT; port <= MAX_PORT; port++)
  {
    if (ctx->bound_ports[port].nsocks == 0)
      return port;
  }

  return 0;
}

/*** Socket Helpers ***********************************************************/

static void sock_conn_reset(struct tcp_sock *sock)
{
  sock->flags = 0;
  sock->remote_ip = 0;
  sock->remote_port = 0;
  sock->tx_seq = 0;
  sock->tx_pending = 0;
  sock->rx_seq = 0;
  sock->tx_remote_avail = 0;
  sock->tx_rexmit_seq = 0;
  sock->tx_rexmit_end_seq = 0;
  sock->rx_dupack_cnt = 0;
  sock->ts_recent = 0;
  sock->rtt_est = 0;
  sock->tx_ready_tsc = 0;
}

static void sock_autobind_drop(struct tcp_slow_context *ctx, struct tcp_sock *sock)
{
  if (!tcp_sock_meta(ctx, sock)->auto_bound)
    return;

  tcp_bound_remove(ctx, sock->local_port, sock->id);
  sock->local_port = 0;
  sock->local_ip = ctx->proto->local_ip;
  tcp_sock_meta(ctx, sock)->auto_bound = 0;
}

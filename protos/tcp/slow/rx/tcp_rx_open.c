#include <errno.h>

#include "tcp_internal.h"

/*** Open Helpers *************************************************************/

static int sock_ack_valid(const struct tcp_sock *sock, __u32 ack);
static void sock_active_established(struct tcp_slow_context *ctx,
    struct tcp_sock *sock, const struct tcp_rx_ctl *rx);
static int sock_passive_established(struct tcp_slow_context *ctx,
    struct tcp_sock *sock, const struct tcp_rx_ctl *rx);

/*** Open API *****************************************************************/

int tcp_rx_syn_sent(struct tcp_slow_context *ctx, struct tcp_sock *sock,
    const struct tcp_rx_ctl *rx)
{
  if ((rx->flags & TAS_TCP_RST) != 0)
  {
    tcp_sock_connect_fail(ctx, sock, ECONNREFUSED);
    return 0;
  }

  if ((rx->flags & (TAS_TCP_SYN | TAS_TCP_ACK)) !=
      (TAS_TCP_SYN | TAS_TCP_ACK))
  {
    return 0;
  }
  if (!sock_ack_valid(sock, rx->ack))
    return 0;

  sock_active_established(ctx, sock, rx);
  tcp_ctl_tx(ctx, sock, TAS_TCP_ACK);
  tcp_app_connect_res(tcp_sock_actx(ctx, sock), sock->opaque, 0, sock);
  return 0;
}

int tcp_rx_syn_recv(struct tcp_slow_context *ctx, struct tcp_sock *sock,
    const struct tcp_rx_ctl *rx)
{
  __u32 listener_id;
  struct tcp_sock *listen_sock;

  if ((rx->flags & TAS_TCP_RST) != 0)
  {
    tcp_sock_close_final(ctx, sock);
    return 0;
  }

  if ((rx->flags & TAS_TCP_SYN) != 0 && (rx->flags & TAS_TCP_ACK) == 0)
  {
    tcp_ctl_tx_resend(ctx, sock, TAS_TCP_SYN | TAS_TCP_ACK |
        ((sock->flags & TCP_SOCK_FLAG_ECN) != 0 ? TAS_TCP_ECE : 0));
    return 0;
  }

  if ((rx->flags & TAS_TCP_ACK) == 0 || !sock_ack_valid(sock, rx->ack))
    return 0;
  if (sock_passive_established(ctx, sock, rx) != 0)
    return 0;

  listener_id = tcp_sock_meta(ctx, sock)->listener_id;
  listen_sock = &tcp_sock_map(ctx)[listener_id];
  tcp_app_listen_newconn(ctx, listen_sock, sock);
  return 0;
}

int tcp_rx_listen_syn(struct tcp_slow_context *ctx, struct tcp_sock *listen_sock,
    const struct tcp_rx_ctl *rx)
{
  int ret;
  struct tcp_listener_slow *listener;
  struct tcp_sock *sock;

  listener = &ctx->listeners[listen_sock->id];
  if (!listener->active || listener->backlog_used >= listener->backlog_len)
  {
    tcp_ctl_tx_reply(ctx, rx->local_ip, rx->local_port, rx->remote_ip,
        rx->remote_port, 0, rx->seq + 1, TAS_TCP_RST | TAS_TCP_ACK);
    return 0;
  }
  if (tcp_flow_lookup(ctx, rx->local_ip, rx->local_port, rx->remote_ip,
          rx->remote_port) != NULL)
  {
    return 0;
  }

  ret = tcp_sock_alloc(ctx, 0, 0, 0, 0, &sock);
  if (ret != 0)
    return -1;

  sock->local_ip = rx->local_ip;
  sock->local_port = rx->local_port;
  sock->remote_ip = rx->remote_ip;
  sock->remote_port = rx->remote_port;
  sock->tx_seq = 1;
  sock->tx_pending = 1;
  sock->rx_seq = rx->seq + 1;
  sock->tx_remote_avail = rx->wnd;
  sock->state = TCP_SOCK_STATE_SYN_RECV;
  tcp_sock_meta(ctx, sock)->listener_id = listen_sock->id;
  if (tcp_cc_ecn_enabled(ctx) &&
      (rx->flags & (TAS_TCP_ECE | TAS_TCP_CWR)) == (TAS_TCP_ECE | TAS_TCP_CWR))
  {
    sock->flags |= TCP_SOCK_FLAG_ECN;
  }

  ret = tcp_flow_insert(ctx, sock);
  if (ret != 0)
  {
    sock->state = TCP_SOCK_STATE_CLOSED;
    tcp_ctl_tx_reply(ctx, rx->local_ip, rx->local_port, rx->remote_ip,
        rx->remote_port, 0, rx->seq + 1, TAS_TCP_RST | TAS_TCP_ACK);
    return -1;
  }

  listener->backlog_used++;
  ret = tcp_ctl_tx(ctx, sock, TAS_TCP_SYN | TAS_TCP_ACK |
      ((sock->flags & TCP_SOCK_FLAG_ECN) != 0 ? TAS_TCP_ECE : 0));
  if (ret != 0)
  {
    tcp_sock_close_final(ctx, sock);
    tcp_ctl_tx_reply(ctx, rx->local_ip, rx->local_port, rx->remote_ip,
        rx->remote_port, 0, rx->seq + 1, TAS_TCP_RST | TAS_TCP_ACK);
    return -1;
  }

  ret = tcp_timeout_arm(ctx, sock, TCP_RETX_SYNACK);
  if (ret != 0)
  {
    tcp_ctl_tx_reply(ctx, rx->local_ip, rx->local_port, rx->remote_ip,
        rx->remote_port, sock->tx_seq, sock->rx_seq, TAS_TCP_RST | TAS_TCP_ACK);
    tcp_sock_close_final(ctx, sock);
    return -1;
  }

  return 0;
}

/*** Open Helpers *************************************************************/

static int sock_ack_valid(const struct tcp_sock *sock, __u32 ack)
{
  return ack == sock->tx_seq + sock->tx_pending;
}

static void sock_active_established(struct tcp_slow_context *ctx,
    struct tcp_sock *sock, const struct tcp_rx_ctl *rx)
{
  tcp_timeout_cancel(ctx, sock);
  if ((rx->flags & TAS_TCP_ECE) == 0)
    sock->flags &= ~TCP_SOCK_FLAG_ECN;
  sock->rx_seq = rx->seq + 1;
  sock->tx_seq += 1;
  sock->tx_remote_avail = rx->wnd;
  sock->tx_pending -= 1;
  sock->state = TCP_SOCK_STATE_ESTABLISHED;
  tcp_sock_ack_progress(sock);
  tcp_sock_cc_init(ctx, sock);
}

static int sock_passive_established(struct tcp_slow_context *ctx,
    struct tcp_sock *sock, const struct tcp_rx_ctl *rx)
{
  __u32 listener_id;

  tcp_timeout_cancel(ctx, sock);
  sock->tx_remote_avail = rx->wnd;
  sock->tx_pending -= 1;
  sock->tx_seq += 1;
  sock->state = TCP_SOCK_STATE_ACCEPT_PENDING;
  tcp_sock_ack_progress(sock);

  listener_id = tcp_sock_meta(ctx, sock)->listener_id;
  if (tcp_listener_ready_push(ctx, listener_id, sock->id) != 0)
  {
    tcp_sock_close_final(ctx, sock);
    return -1;
  }

  return 0;
}

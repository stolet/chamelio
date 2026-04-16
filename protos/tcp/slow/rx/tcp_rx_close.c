#include "tcp_internal.h"

/*** Close API ****************************************************************/

int tcp_rx_established(struct tcp_slow_context *ctx, struct tcp_sock *sock,
    const struct tcp_rx_ctl *rx)
{
  if ((rx->flags & TAS_TCP_RST) != 0)
  {
    tcp_sock_close_final(ctx, sock);
    return 0;
  }

  if ((rx->flags & TAS_TCP_FIN) != 0)
  {
    sock->rx_seq = rx->seq + 1;
    tcp_ctl_tx(ctx, sock, TAS_TCP_ACK);
    tcp_sock_close_final(ctx, sock);
  }

  return 0;
}

int tcp_rx_fin_wait1(struct tcp_slow_context *ctx, struct tcp_sock *sock,
    const struct tcp_rx_ctl *rx)
{
  if ((rx->flags & TAS_TCP_FIN) != 0)
  {
    sock->rx_seq = rx->seq + 1;
    tcp_ctl_tx(ctx, sock, TAS_TCP_ACK);
  }

  if ((rx->flags & TAS_TCP_ACK) != 0 && rx->ack == sock->tx_seq)
  {
    tcp_timeout_cancel(ctx, sock);
    tcp_sock_close_final(ctx, sock);
    return 0;
  }

  if ((rx->flags & TAS_TCP_FIN) != 0)
  {
    tcp_timeout_cancel(ctx, sock);
    tcp_sock_close_final(ctx, sock);
  }

  return 0;
}

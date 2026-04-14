#include <errno.h>

#include "clock.h"
#include "tcp_internal.h"
#include "tcp_hdr.h"
#include "log.h"

/*** Timeout Helpers **********************************************************/

static int sock_retx_sched(struct tcp_slow_context *ctx, struct tcp_sock *sock);
static __u32 sock_retx_seq(const struct tcp_sock *sock);
static int sock_retx_flags(const struct tcp_sock *sock, __u8 kind, __u16 *flags);

/*** Socket Helpers ***********************************************************/

static int sock_retx_tx(struct tcp_slow_context *ctx, struct tcp_sock *sock);
static void sock_retx_expire(struct tcp_slow_context *ctx, struct tcp_sock *sock);
static int sock_on_timeout(struct tcp_slow_context *ctx, struct tcp_sock *sock);

/*** Public API ***************************************************************/

int tcp_timeout_poll(struct tcp_slow_context *ctx)
{
  int i;
  struct to_entry *te;
  struct tcp_sock *sock;
  struct tcp_sock_meta_slow *meta;

  for (i = 0; i < SLOW_BATCH_SIZE; i++)
  {
    te = tomgr_peek(ctx->tomgr);
    if (te == NULL || te->to >= clock_rdtsc())
      break;

    te = tomgr_pop(ctx->tomgr);
    if (te == NULL)
    {
      LOG_ERROR("failed to pop timeout manager");
      return -1;
    }

    sock = te->data;
    meta = &ctx->sock_meta[sock->id];
    meta->timer = NULL;

    if (meta->retx_kind == TCP_RETX_NONE)
      continue;

    sock_on_timeout(ctx, sock);
  }

  return 0;
}

int tcp_timeout_arm(struct tcp_slow_context *ctx, struct tcp_sock *sock,
    __u8 kind)
{
  struct tcp_sock_meta_slow *meta;

  meta = &ctx->sock_meta[sock->id];
  tcp_timeout_cancel(ctx, sock);
  meta->retx_kind = kind;
  meta->retx_left = TCP_RETX_RETRIES;
  return sock_retx_sched(ctx, sock);
}

void tcp_timeout_cancel(struct tcp_slow_context *ctx, struct tcp_sock *sock)
{
  struct tcp_sock_meta_slow *meta;

  meta = &ctx->sock_meta[sock->id];
  if (meta->timer != NULL)
  {
    tomgr_cancel(ctx->tomgr, meta->timer);
    meta->timer = NULL;
  }
  meta->retx_kind = TCP_RETX_NONE;
  meta->retx_left = 0;
}

/*** Timeout Helpers **********************************************************/

static int sock_retx_sched(struct tcp_slow_context *ctx, struct tcp_sock *sock)
{
  struct tcp_sock_meta_slow *meta;

  meta = &ctx->sock_meta[sock->id];
  meta->timer = tomgr_insert(ctx->tomgr, TCP_TO_RETX,
      clock_tsc_after_us(TCP_RETX_US), sock);
  if (meta->timer == NULL)
  {
    LOG_WARN("failed to schedule TCP retransmission timeout");
    meta->retx_kind = TCP_RETX_NONE;
    meta->retx_left = 0;
    return -1;
  }

  return 0;
}

static __u32 sock_retx_seq(const struct tcp_sock *sock)
{
  return sock->tx_seq + sock->tx_pending - 1;
}

static int sock_retx_flags(const struct tcp_sock *sock, __u8 kind, __u16 *flags)
{
  switch (kind)
  {
    case TCP_RETX_SYN:
      *flags = TAS_TCP_SYN |
          ((sock->flags & TCP_SOCK_FLAG_ECN) != 0 ?
          TAS_TCP_ECE | TAS_TCP_CWR : 0);
      return 0;
    case TCP_RETX_SYNACK:
      *flags = TAS_TCP_SYN | TAS_TCP_ACK |
          ((sock->flags & TCP_SOCK_FLAG_ECN) != 0 ? TAS_TCP_ECE : 0);
      return 0;
    case TCP_RETX_FIN:
      *flags = TAS_TCP_FIN | TAS_TCP_ACK;
      return 0;
    default:
      return -1;
  }
}

/*** Socket Helpers ***********************************************************/

static int sock_retx_tx(struct tcp_slow_context *ctx, struct tcp_sock *sock)
{
  __u16 flags;
  struct tcp_sock_meta_slow *meta;

  meta = &ctx->sock_meta[sock->id];
  if (sock_retx_flags(sock, meta->retx_kind, &flags) != 0)
    return -1;

  return tcp_ctrl_tx_reply(ctx, sock->local_ip, sock->local_port,
      sock->remote_ip, sock->remote_port, sock_retx_seq(sock),
      sock->rx_seq, flags);
}

static void sock_retx_expire(struct tcp_slow_context *ctx, struct tcp_sock *sock)
{
  struct tcp_sock_meta_slow *meta;

  meta = &ctx->sock_meta[sock->id];
  switch (meta->retx_kind)
  {
    case TCP_RETX_SYN:
      tcp_sock_connect_fail(ctx, sock, ETIMEDOUT);
      break;
    case TCP_RETX_SYNACK:
      tcp_ctrl_tx_reply(ctx, sock->local_ip, sock->local_port,
          sock->remote_ip, sock->remote_port, sock->tx_seq, sock->rx_seq,
          TAS_TCP_RST | TAS_TCP_ACK);
      tcp_sock_close_final(ctx, sock);
      break;
    case TCP_RETX_FIN:
      tcp_ctrl_tx_reply(ctx, sock->local_ip, sock->local_port,
          sock->remote_ip, sock->remote_port, sock->tx_seq, sock->rx_seq,
          TAS_TCP_RST | TAS_TCP_ACK);
      tcp_sock_close_final(ctx, sock);
      break;
    default:
      break;
  }
}

static int sock_on_timeout(struct tcp_slow_context *ctx, struct tcp_sock *sock)
{
  struct tcp_sock_meta_slow *meta;

  meta = &ctx->sock_meta[sock->id];
  if (meta->retx_left == 0)
  {
    sock_retx_expire(ctx, sock);
    return 0;
  }

  if (sock_retx_tx(ctx, sock) != 0)
  {
    sock_retx_expire(ctx, sock);
    return -1;
  }

  meta->retx_left--;
  if (sock_retx_sched(ctx, sock) != 0)
  {
    sock_retx_expire(ctx, sock);
    return -1;
  }

  return 0;
}

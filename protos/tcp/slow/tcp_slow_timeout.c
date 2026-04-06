#include <errno.h>

#include "clock.h"
#include "tcp_slow_internal.h"
#include "tcp_hdr.h"
#include "log.h"

static int schedule_timeout(struct tcp_slow_context *ctx, struct tcp_sock *sock);
static int handle_retx_timeout(struct tcp_slow_context *ctx,
    struct tcp_sock *sock);
static int retransmit_ctrl(struct tcp_slow_context *ctx, struct tcp_sock *sock);
static void expire_retx(struct tcp_slow_context *ctx, struct tcp_sock *sock);

int tcp_slow_timeout_poll(struct tcp_slow_context *ctx)
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

    if (meta->retx_kind == TCP_SLOW_RETX_NONE)
      continue;

    handle_retx_timeout(ctx, sock);
  }

  return 0;
}

int tcp_slow_timeout_arm(struct tcp_slow_context *ctx, struct tcp_sock *sock,
    __u8 kind)
{
  struct tcp_sock_meta_slow *meta;

  meta = &ctx->sock_meta[sock->id];
  tcp_slow_timeout_cancel(ctx, sock);
  meta->retx_kind = kind;
  meta->retx_left = TCP_SLOW_RETX_RETRIES;
  return schedule_timeout(ctx, sock);
}

void tcp_slow_timeout_cancel(struct tcp_slow_context *ctx, struct tcp_sock *sock)
{
  struct tcp_sock_meta_slow *meta;

  meta = &ctx->sock_meta[sock->id];
  if (meta->timer != NULL)
  {
    tomgr_cancel(ctx->tomgr, meta->timer);
    meta->timer = NULL;
  }
  meta->retx_kind = TCP_SLOW_RETX_NONE;
  meta->retx_left = 0;
}

static int schedule_timeout(struct tcp_slow_context *ctx, struct tcp_sock *sock)
{
  struct tcp_sock_meta_slow *meta;

  meta = &ctx->sock_meta[sock->id];
  meta->timer = tomgr_insert(ctx->tomgr, TCP_TO_RETX,
      clock_tsc_after_us(TCP_SLOW_RETX_US), sock);
  if (meta->timer == NULL)
  {
    LOG_WARN("failed to schedule TCP retransmission timeout");
    meta->retx_kind = TCP_SLOW_RETX_NONE;
    meta->retx_left = 0;
    return -1;
  }

  return 0;
}

static int handle_retx_timeout(struct tcp_slow_context *ctx,
    struct tcp_sock *sock)
{
  struct tcp_sock_meta_slow *meta;

  meta = &ctx->sock_meta[sock->id];
  if (meta->retx_left == 0)
  {
    expire_retx(ctx, sock);
    return 0;
  }

  if (retransmit_ctrl(ctx, sock) != 0)
  {
    expire_retx(ctx, sock);
    return -1;
  }

  meta->retx_left--;
  if (schedule_timeout(ctx, sock) != 0)
  {
    expire_retx(ctx, sock);
    return -1;
  }

  return 0;
}

static int retransmit_ctrl(struct tcp_slow_context *ctx, struct tcp_sock *sock)
{
  struct tcp_sock_meta_slow *meta;
  __u32 seq;
  __u16 flags;

  meta = &ctx->sock_meta[sock->id];
  switch (meta->retx_kind)
  {
    case TCP_SLOW_RETX_SYN:
      seq = sock->tx_seq - 1;
      flags = TAS_TCP_SYN;
      break;
    case TCP_SLOW_RETX_SYNACK:
      seq = sock->tx_seq - 1;
      flags = TAS_TCP_SYN | TAS_TCP_ACK;
      break;
    case TCP_SLOW_RETX_FIN:
      seq = sock->tx_seq - 1;
      flags = TAS_TCP_FIN | TAS_TCP_ACK;
      break;
    default:
      return -1;
  }

  return tcp_slow_state_enqueue_ctrl_reply(ctx, sock->local_ip,
      sock->local_port, sock->remote_ip, sock->remote_port, seq,
      sock->rx_seq, flags);
}

static void expire_retx(struct tcp_slow_context *ctx, struct tcp_sock *sock)
{
  struct tcp_sock_meta_slow *meta;

  meta = &ctx->sock_meta[sock->id];
  switch (meta->retx_kind)
  {
    case TCP_SLOW_RETX_SYN:
      tcp_slow_state_sock_connect_failed(ctx, sock, ETIMEDOUT);
      break;
    case TCP_SLOW_RETX_SYNACK:
      tcp_slow_state_enqueue_ctrl_reply(ctx, sock->local_ip, sock->local_port,
          sock->remote_ip, sock->remote_port, sock->tx_seq, sock->rx_seq,
          TAS_TCP_RST | TAS_TCP_ACK);
      tcp_slow_state_sock_close_final(ctx, sock);
      break;
    case TCP_SLOW_RETX_FIN:
      tcp_slow_state_enqueue_ctrl_reply(ctx, sock->local_ip, sock->local_port,
          sock->remote_ip, sock->remote_port, sock->tx_seq, sock->rx_seq,
          TAS_TCP_RST | TAS_TCP_ACK);
      tcp_slow_state_sock_close_final(ctx, sock);
      break;
    default:
      break;
  }
}

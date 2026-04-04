#include <errno.h>

#include "tcp_slow_internal.h"
#include "queue_fns.h"
#include "tcp_hdr.h"
#include "log.h"

static int handle_ctrl_rx(struct tcp_slow_context *ctx,
    struct tcp_pkt_inner *pkt);
static int handle_listen_syn(struct tcp_slow_context *ctx,
    struct tcp_sock *listen_sock, __u32 local_ip, __u16 local_port,
    __u32 remote_ip, __u16 remote_port, __u32 seq);

int tcp_slow_fast_poll(struct tcp_slow_context *ctx)
{
  int n;
  struct tcp_queue_bump_entry *sig_qe;
  struct tcp_queue_bump_entry *pkt_qe;

  n = 0;
  while (n < SLOW_BATCH_SIZE)
  {
    sig_qe = queue_head(ctx->fast_slow_sig_q);
    if (sig_qe == NULL)
      break;

    n++;
    switch (sig_qe->type)
    {
      case TCP_QUEUE_CTRL_RX:
        pkt_qe = queue_head(ctx->fast_slow_pkt_q);
        if (pkt_qe == NULL)
        {
          LOG_WARN("missing fast->slow TCP control packet for signal");
          queue_dequeue(ctx->fast_slow_sig_q);
          break;
        }
        if (pkt_qe->type != TCP_QUEUE_CTRL_RX_PKT)
        {
          LOG_WARN("unexpected fast->slow TCP control packet type=%d",
              pkt_qe->type);
          queue_dequeue(ctx->fast_slow_sig_q);
          queue_dequeue(ctx->fast_slow_pkt_q);
          break;
        }

        handle_ctrl_rx(ctx, &pkt_qe->data.ctrl_pkt.pkt);
        queue_dequeue(ctx->fast_slow_pkt_q);
        break;
      default:
        LOG_WARN("unknown queue entry type from fast to tcp slow-path type=%d",
            sig_qe->type);
        break;
    }
    queue_dequeue(ctx->fast_slow_sig_q);
  }

  return n;
}

static int handle_ctrl_rx(struct tcp_slow_context *ctx,
    struct tcp_pkt_inner *pkt)
{
  __u32 local_ip;
  __u32 remote_ip;
  __u32 seq;
  __u32 ack;
  __u16 local_port;
  __u16 remote_port;
  __u16 flags;
  struct tcp_sock *sock;
  struct tcp_sock *listen_sock;
  __u32 listener_id;
  struct ip_hdr *ip;
  struct tcp_hdr *tcp;

  ip = &pkt->ip;
  tcp = &pkt->tcp;
  local_ip = f_beui32(ip->dst);
  remote_ip = f_beui32(ip->src);
  seq = f_beui32(tcp->seqno);
  ack = f_beui32(tcp->ackno);
  local_port = f_beui16(tcp->dest);
  remote_port = f_beui16(tcp->src);
  flags = TCPH_FLAGS(tcp);

  sock = tcp_slow_state_flow_lookup(ctx, local_ip, local_port, remote_ip,
      remote_port);
  if (sock != NULL)
  {
    switch (sock->state)
    {
      case TCP_SOCK_STATE_SYN_SENT:
        if ((flags & TAS_TCP_RST) != 0)
        {
          tcp_slow_state_sock_connect_failed(ctx, sock, ECONNREFUSED);
          return 0;
        }

        if ((flags & (TAS_TCP_SYN | TAS_TCP_ACK)) !=
            (TAS_TCP_SYN | TAS_TCP_ACK))
          return 0;
        if (ack != sock->tx_seq)
          return 0;

        tcp_slow_timeout_cancel(ctx, sock);
        sock->rx_seq = seq + 1;
        sock->tx_pending -= 1;
        tcp_slow_state_enqueue_ctrl_tx(ctx, sock, TAS_TCP_ACK);
        sock->state = TCP_SOCK_STATE_ESTABLISHED;
        tcp_slow_app_enqueue_connect_res(tcp_slow_sock_actx(ctx, sock),
            sock->opaque, 0, sock);
        return 0;

      case TCP_SOCK_STATE_SYN_RECV:
        if ((flags & TAS_TCP_RST) != 0)
        {
          tcp_slow_state_sock_close_final(ctx, sock);
          return 0;
        }

        if ((flags & TAS_TCP_SYN) != 0 && (flags & TAS_TCP_ACK) == 0)
        {
          tcp_slow_state_enqueue_ctrl_reply(ctx, sock->local_ip,
              sock->local_port, sock->remote_ip, sock->remote_port,
              sock->tx_seq - 1, sock->rx_seq, TAS_TCP_SYN | TAS_TCP_ACK);
          return 0;
        }

        if ((flags & TAS_TCP_ACK) == 0 || ack != sock->tx_seq)
          return 0;

        tcp_slow_timeout_cancel(ctx, sock);
        sock->tx_pending -= 1;
        sock->state = TCP_SOCK_STATE_ACCEPT_PENDING;
        if (tcp_slow_state_listener_ready_push(ctx,
                ctx->sock_meta[sock->id].listener_id, sock->id) != 0)
        {
          tcp_slow_state_sock_close_final(ctx, sock);
          return 0;
        }
        listen_sock = &tcp_slow_get_sock_map(ctx)[
            ctx->sock_meta[sock->id].listener_id];
        tcp_slow_app_enqueue_listen_newconn(ctx, listen_sock, sock);
        return 0;

      case TCP_SOCK_STATE_ACCEPT_PENDING:
      case TCP_SOCK_STATE_ESTABLISHED:
        if ((flags & TAS_TCP_RST) != 0)
        {
          tcp_slow_state_sock_close_final(ctx, sock);
          return 0;
        }

        if ((flags & TAS_TCP_FIN) != 0)
        {
          sock->rx_seq = seq + 1;
          tcp_slow_state_enqueue_ctrl_tx(ctx, sock, TAS_TCP_ACK);
          tcp_slow_state_sock_close_final(ctx, sock);
          return 0;
        }

        return 0;

      case TCP_SOCK_STATE_FIN_WAIT1:
        if ((flags & TAS_TCP_FIN) != 0)
        {
          sock->rx_seq = seq + 1;
          tcp_slow_state_enqueue_ctrl_tx(ctx, sock, TAS_TCP_ACK);
        }

        if ((flags & TAS_TCP_ACK) != 0 && ack == sock->tx_seq)
        {
          tcp_slow_timeout_cancel(ctx, sock);
          tcp_slow_state_sock_close_final(ctx, sock);
          return 0;
        }

        if ((flags & TAS_TCP_FIN) != 0)
        {
          tcp_slow_timeout_cancel(ctx, sock);
          tcp_slow_state_sock_close_final(ctx, sock);
          return 0;
        }
        return 0;

      default:
        return 0;
    }
  }

  if ((flags & TAS_TCP_SYN) == 0 || (flags & TAS_TCP_ACK) != 0)
    return 0;

  listener_id = tcp_slow_state_listener_lookup(ctx, local_ip, local_port);
  if (listener_id == ID_INVALID)
  {
    tcp_slow_state_enqueue_ctrl_reply(ctx, local_ip, local_port, remote_ip,
        remote_port, 0, seq + 1,
        TAS_TCP_RST | TAS_TCP_ACK);
    return 0;
  }

  listen_sock = &tcp_slow_get_sock_map(ctx)[listener_id];
  return handle_listen_syn(ctx, listen_sock, local_ip, local_port, remote_ip,
      remote_port, seq);
}

static int handle_listen_syn(struct tcp_slow_context *ctx,
    struct tcp_sock *listen_sock, __u32 local_ip, __u16 local_port,
    __u32 remote_ip, __u16 remote_port, __u32 seq)
{
  int ret;
  struct tcp_sock *sock;
  struct tcp_listener_slow *listener;

  listener = &ctx->listeners[listen_sock->id];
  if (!listener->active || listener->backlog_used >= listener->backlog_len)
  {
    tcp_slow_state_enqueue_ctrl_reply(ctx, local_ip, local_port, remote_ip,
        remote_port, 0, seq + 1,
        TAS_TCP_RST | TAS_TCP_ACK);
    return 0;
  }

  sock = tcp_slow_state_flow_lookup(ctx, local_ip, local_port, remote_ip,
      remote_port);
  if (sock != NULL)
    return 0;

  ret = tcp_slow_state_alloc_sock(ctx, 0, 0, 0, 0, &sock);
  if (ret != 0)
    return -1;

  sock->local_ip = local_ip;
  sock->local_port = local_port;
  sock->remote_ip = remote_ip;
  sock->remote_port = remote_port;
  sock->tx_seq = 1;
  sock->tx_pending = 1;
  sock->rx_seq = seq + 1;
  sock->state = TCP_SOCK_STATE_SYN_RECV;
  ctx->sock_meta[sock->id].listener_id = listen_sock->id;

  ret = tcp_slow_state_flow_insert(ctx, sock);
  if (ret != 0)
  {
    sock->state = TCP_SOCK_STATE_CLOSED;
    tcp_slow_state_enqueue_ctrl_reply(ctx, local_ip, local_port, remote_ip,
        remote_port, 0, seq + 1,
        TAS_TCP_RST | TAS_TCP_ACK);
    return -1;
  }

  listener->backlog_used++;
  ret = tcp_slow_state_enqueue_ctrl_tx(ctx, sock, TAS_TCP_SYN | TAS_TCP_ACK);
  if (ret != 0)
  {
    tcp_slow_state_sock_close_final(ctx, sock);
    tcp_slow_state_enqueue_ctrl_reply(ctx, local_ip, local_port, remote_ip,
        remote_port, 0, seq + 1,
        TAS_TCP_RST | TAS_TCP_ACK);
    return -1;
  }

  ret = tcp_slow_timeout_arm(ctx, sock, TCP_SLOW_RETX_SYNACK);
  if (ret != 0)
  {
    tcp_slow_state_enqueue_ctrl_reply(ctx, local_ip, local_port, remote_ip,
        remote_port, sock->tx_seq, sock->rx_seq,
        TAS_TCP_RST | TAS_TCP_ACK);
    tcp_slow_state_sock_close_final(ctx, sock);
    return -1;
  }

  return 0;
}

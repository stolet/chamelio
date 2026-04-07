#include <errno.h>

#include "tcp_slow_internal.h"
#include "queue_fns.h"
#include "ip_hdr.h"
#include "tcp_hdr.h"
#include "log.h"

static int handle_ctrl_rx(struct tcp_slow_context *ctx,
    struct tcp_pkt_inner *pkt);
static int handle_listen_syn(struct tcp_slow_context *ctx,
    struct tcp_sock *listen_sock, __u32 local_ip, __u16 local_port,
    __u32 remote_ip, __u16 remote_port, __u32 seq, __u16 wnd, __u16 flags);

static __u64 ctrl_rst_logs;
static __u64 ctrl_fin_logs;
static __u64 ctrl_handshake_logs;
static __u64 ctrl_unexpected_logs;

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
  __u16 ip_total_len;
  __u16 local_port;
  __u16 remote_port;
  __u16 tcp_hdrs_len;
  __u16 payload_len;
  __u16 wnd;
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
  ip_total_len = f_beui16(ip->len);
  local_port = f_beui16(tcp->dest);
  remote_port = f_beui16(tcp->src);
  tcp_hdrs_len = (__u16) (TCPH_HDRLEN(tcp) * 4);
  payload_len = ip_total_len > sizeof(struct ip_hdr) + tcp_hdrs_len ?
      ip_total_len - sizeof(struct ip_hdr) - tcp_hdrs_len : 0;
  wnd = f_beui16(tcp->wnd);
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
          if (tcp_slow_should_log(&ctrl_handshake_logs))
          {
            LOG_WARN("tcp control RST during connect sock=%u state=%s "
                "local=%u:%u remote=%u:%u seq=%u ack=%u wnd=%u",
                sock->id, tcp_slow_state_name(sock->state), local_ip,
                local_port, remote_ip, remote_port, seq, ack, wnd);
          }
          tcp_slow_state_sock_connect_failed(ctx, sock, ECONNREFUSED);
          return 0;
        }

        if ((flags & (TAS_TCP_SYN | TAS_TCP_ACK)) !=
            (TAS_TCP_SYN | TAS_TCP_ACK))
          return 0;
        if (ack != sock->tx_seq)
          return 0;

        tcp_slow_timeout_cancel(ctx, sock);
        if ((flags & TAS_TCP_ECE) == 0)
          sock->flags &= ~TCP_SOCK_FLAG_ECN;
        sock->rx_seq = seq + 1;
        sock->tx_remote_avail = wnd;
        sock->tx_pending -= 1;
        tcp_slow_state_enqueue_ctrl_tx(ctx, sock, TAS_TCP_ACK);
        sock->state = TCP_SOCK_STATE_ESTABLISHED;
        tcp_slow_cc_init_sock(ctx, sock);
        tcp_slow_app_enqueue_connect_res(tcp_slow_sock_actx(ctx, sock),
            sock->opaque, 0, sock);
        return 0;

      case TCP_SOCK_STATE_SYN_RECV:
        if ((flags & TAS_TCP_RST) != 0)
        {
          if (tcp_slow_should_log(&ctrl_handshake_logs))
          {
            LOG_WARN("tcp control RST during accept handshake sock=%u state=%s "
                "local=%u:%u remote=%u:%u seq=%u ack=%u wnd=%u",
                sock->id, tcp_slow_state_name(sock->state), local_ip,
                local_port, remote_ip, remote_port, seq, ack, wnd);
          }
          tcp_slow_state_sock_close_final(ctx, sock);
          return 0;
        }

        if ((flags & TAS_TCP_SYN) != 0 && (flags & TAS_TCP_ACK) == 0)
        {
          if (tcp_slow_should_log(&ctrl_handshake_logs))
          {
            LOG_WARN("tcp duplicate SYN for half-open socket sock=%u state=%s "
                "local=%u:%u remote=%u:%u seq=%u ack=%u wnd=%u",
                sock->id, tcp_slow_state_name(sock->state), local_ip,
                local_port, remote_ip, remote_port, seq, ack, wnd);
          }
          tcp_slow_state_enqueue_ctrl_resend(ctx, sock,
              TAS_TCP_SYN | TAS_TCP_ACK |
              ((sock->flags & TCP_SOCK_FLAG_ECN) != 0 ? TAS_TCP_ECE : 0));
          return 0;
        }

        if ((flags & TAS_TCP_ACK) == 0 || ack != sock->tx_seq)
          return 0;

        tcp_slow_timeout_cancel(ctx, sock);
        sock->tx_remote_avail = wnd;
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
          if (tcp_slow_should_log(&ctrl_rst_logs))
          {
            LOG_WARN("tcp control RST closing socket sock=%u state=%s "
                "local=%u:%u remote=%u:%u seq=%u ack=%u wnd=%u tx_seq=%u "
                "tx_pending=%u rx_seq=%u",
                sock->id, tcp_slow_state_name(sock->state), local_ip,
                local_port, remote_ip, remote_port, seq, ack, wnd,
                sock->tx_seq, sock->tx_pending, sock->rx_seq);
          }
          tcp_slow_state_sock_close_final(ctx, sock);
          return 0;
        }

        if ((flags & TAS_TCP_FIN) != 0)
        {
          if (tcp_slow_should_log(&ctrl_fin_logs))
          {
            LOG_WARN("tcp control FIN closing socket sock=%u state=%s "
                "local=%u:%u remote=%u:%u seq=%u ack=%u wnd=%u tx_seq=%u "
                "tx_pending=%u rx_seq=%u",
                sock->id, tcp_slow_state_name(sock->state), local_ip,
                local_port, remote_ip, remote_port, seq, ack, wnd,
                sock->tx_seq, sock->tx_pending, sock->rx_seq);
          }
          sock->rx_seq = seq + 1;
          tcp_slow_state_enqueue_ctrl_tx(ctx, sock, TAS_TCP_ACK);
          tcp_slow_state_sock_close_final(ctx, sock);
          return 0;
        }

        if (tcp_slow_should_log(&ctrl_unexpected_logs))
        {
          LOG_WARN("unexpected fast->slow ESTABLISHED packet sock=%u state=%s "
              "local=%u:%u remote=%u:%u flags=0x%x seq=%u ack=%u "
              "payload=%u tx_seq=%u tx_pending=%u rx_seq=%u",
              sock->id, tcp_slow_state_name(sock->state), local_ip,
              local_port, remote_ip, remote_port, flags, seq, ack,
              payload_len, sock->tx_seq, sock->tx_pending, sock->rx_seq);
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
      remote_port, seq, wnd, flags);
}

static int handle_listen_syn(struct tcp_slow_context *ctx,
    struct tcp_sock *listen_sock, __u32 local_ip, __u16 local_port,
    __u32 remote_ip, __u16 remote_port, __u32 seq, __u16 wnd, __u16 flags)
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
  sock->tx_remote_avail = wnd;
  sock->state = TCP_SOCK_STATE_SYN_RECV;
  ctx->sock_meta[sock->id].listener_id = listen_sock->id;
  if (tcp_slow_cc_ecn_enabled(ctx) &&
      (flags & (TAS_TCP_ECE | TAS_TCP_CWR)) == (TAS_TCP_ECE | TAS_TCP_CWR))
  {
    sock->flags |= TCP_SOCK_FLAG_ECN;
  }

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
  ret = tcp_slow_state_enqueue_ctrl_tx(ctx, sock, TAS_TCP_SYN | TAS_TCP_ACK |
      ((sock->flags & TCP_SOCK_FLAG_ECN) != 0 ? TAS_TCP_ECE : 0));
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

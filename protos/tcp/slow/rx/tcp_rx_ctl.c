#include "tcp_internal.h"
#include "queue_fns.h"
#include "ip_hdr.h"
#include "tcp_hdr.h"
#include "log.h"
#include "utils_sync.h"
#include "clock.h"

/*** RX Helpers ***************************************************************/

static int ctl_rx(struct tcp_slow_context *ctx, struct tcp_pkt_inner *pkt);
static void ctl_rx_parse(const struct tcp_pkt_inner *pkt,
    struct tcp_rx_ctl *rx);
static void sock_ts_rx(struct tcp_sock *sock, const struct tcp_rx_ctl *rx);
static int sock_ctl_rx(struct tcp_slow_context *ctx, struct tcp_sock *sock,
    const struct tcp_rx_ctl *rx);

/*** RX Poll ******************************************************************/

int tcp_fast_poll(struct tcp_slow_context *ctx)
{
  int nr;
  struct tcp_queue_ctl_entry *sig_qe;
  struct tcp_queue_pkt_entry *pkt_qe;

  nr = 0;
  while (nr < SLOW_BATCH_SIZE)
  {
    sig_qe = queue_head(ctx->fast_slow_sig_q);
    if (sig_qe == NULL)
      break;

    nr++;
    switch (sig_qe->type)
    {
      case TCP_QUEUE_CTL_RX:
        pkt_qe = queue_head(ctx->fast_slow_pkt_q);
        if (pkt_qe == NULL)
        {
          LOG_WARN("missing fast->slow TCP control packet for signal");
          queue_dequeue(ctx->fast_slow_sig_q);
          break;
        }
        if (pkt_qe->type != TCP_QUEUE_CTL_RX_PKT)
        {
          LOG_WARN("unexpected fast->slow TCP control packet type=%d",
              pkt_qe->type);
          queue_dequeue(ctx->fast_slow_sig_q);
          queue_dequeue(ctx->fast_slow_pkt_q);
          break;
        }

        ctl_rx(ctx, &pkt_qe->data.ctl_pkt.pkt);
        queue_dequeue(ctx->fast_slow_pkt_q);
        break;
      default:
        LOG_WARN("unknown queue entry type from fast to tcp slow-path type=%d",
            sig_qe->type);
        break;
    }
    queue_dequeue(ctx->fast_slow_sig_q);
  }

  return nr;
}

/*** RX Helpers ***************************************************************/

static int ctl_rx(struct tcp_slow_context *ctx, struct tcp_pkt_inner *pkt)
{
  __u32 listener_id;
  /* TODO: Extra structure just to parse rx seems unecessary */
  struct tcp_rx_ctl rx;
  struct tcp_sock *listen_sock;
  struct tcp_sock *sock;

  ctl_rx_parse(pkt, &rx);
  sock = tcp_flow_lookup(ctx, rx.local_ip, rx.local_port, rx.remote_ip,
      rx.remote_port);
  if (sock != NULL)
  {
    util_spin_lock(&sock->lock);
    sock_ts_rx(sock, &rx);
    sock_ctl_rx(ctx, sock, &rx);
    util_spin_unlock(&sock->lock);
    return 0;
  }

  if ((rx.flags & TAS_TCP_SYN) == 0 || (rx.flags & TAS_TCP_ACK) != 0)
    return 0;

  listener_id = tcp_listener_lookup(ctx, rx.local_ip, rx.local_port);
  if (listener_id == ID_INVALID)
  {
    tcp_ctl_tx_reply(ctx, rx.local_ip, rx.local_port, rx.remote_ip,
        rx.remote_port, 0, rx.seq + 1, TAS_TCP_RST | TAS_TCP_ACK);
    return 0;
  }

  listen_sock = &tcp_sock_map(ctx)[listener_id];
  return tcp_rx_listen_syn(ctx, listen_sock, &rx);
}

static void ctl_rx_parse(const struct tcp_pkt_inner *pkt, struct tcp_rx_ctl *rx)
{
  const struct tcp_timestamp_opt *ts_opt;
  const struct ip_hdr *ip;
  const struct tcp_hdr *tcp;
  __u32 hdrlen;

  ip = &pkt->ip;
  tcp = &pkt->tcp;
  hdrlen = TCPH_HDRLEN(tcp) * 4;
  rx->local_ip = f_beui32(ip->dst);
  rx->remote_ip = f_beui32(ip->src);
  rx->seq = f_beui32(tcp->seqno);
  rx->ack = f_beui32(tcp->ackno);
  rx->local_port = f_beui16(tcp->dest);
  rx->remote_port = f_beui16(tcp->src);
  rx->wnd = f_beui16(tcp->wnd);
  rx->flags = TCPH_FLAGS(tcp);
  rx->ts_valid = 0;
  if (hdrlen < TCP_HLEN + TCP_TS_OPT_LEN)
    return;

  ts_opt = (const struct tcp_timestamp_opt *) ((__u8 *) tcp + TCP_HLEN);
  if (ts_opt->kind != TCP_OPT_TIMESTAMP)
    return;
  
  rx->ts_valid = 1;
  rx->ts_val = f_beui32(ts_opt->ts_val);
  rx->ts_ecr = f_beui32(ts_opt->ts_ecr);
}

static void sock_ts_rx(struct tcp_sock *sock, const struct tcp_rx_ctl *rx)
{
  __u32 now_us;
  __u32 rtt;

  if (!rx->ts_valid)
    return;

  sock->ts_recent = rx->ts_val;
  if (rx->ts_ecr == 0)
    return;

  now_us = (__u32) clock_tsc_to_us(clock_rdtsc());
  rtt = now_us - rx->ts_ecr;
  if (rtt == 0 || rtt >= TCP_MAX_RTT)
    return;

  if (sock->rtt_est != 0)
    sock->rtt_est = (7 * sock->rtt_est + rtt) / 8;
  else
    sock->rtt_est = rtt;
}

static int sock_ctl_rx(struct tcp_slow_context *ctx, struct tcp_sock *sock,
    const struct tcp_rx_ctl *rx)
{
  switch (sock->state)
  {
    case TCP_SOCK_STATE_SYN_SENT:
      return tcp_rx_syn_sent(ctx, sock, rx);
    case TCP_SOCK_STATE_SYN_RECV:
      return tcp_rx_syn_recv(ctx, sock, rx);
    case TCP_SOCK_STATE_ACCEPT_PENDING:
    case TCP_SOCK_STATE_ESTABLISHED:
      return tcp_rx_established(ctx, sock, rx);
    case TCP_SOCK_STATE_FIN_WAIT1:
      return tcp_rx_fin_wait1(ctx, sock, rx);
    default:
      return 0;
  }
}

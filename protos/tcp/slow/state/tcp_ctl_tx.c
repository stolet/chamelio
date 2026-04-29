#include "tcp_internal.h"
#include "queue_fns.h"
#include "tcp_hdr.h"
#include "clock.h"

/*** Ctrl TX Helpers **********************************************************/

static int ctl_pkt_enqueue(struct tcp_slow_context *ctx, const struct tcp_sock *sock,
    __u32 local_ip,
    __u16 local_port, __u32 remote_ip, __u16 remote_port, __u32 seq,
    __u32 ack, __u16 flags, __u16 wnd);
static void ctl_pkt_fill(struct tcp_queue_ctl_pkt *pkt, const struct tcp_sock *sock,
    __u32 local_ip,
    __u16 local_port, __u32 remote_ip, __u16 remote_port, __u32 seq,
    __u32 ack, __u16 flags, __u16 wnd);

/*** Ctrl TX API **************************************************************/

int tcp_ctl_tx(struct tcp_slow_context *ctx, struct tcp_sock *sock, __u16 flags)
{
  return ctl_pkt_enqueue(ctx, sock, sock->local_ip, sock->local_port, sock->remote_ip,
      sock->remote_port, sock->tx_seq, sock->rx_seq, flags,
      tcp_sock_rx_wnd(sock));
}

int tcp_ctl_tx_resend(struct tcp_slow_context *ctx, struct tcp_sock *sock,
    __u16 flags)
{
  __u32 seq;

  seq = sock->tx_seq;
  if ((flags & (TAS_TCP_SYN | TAS_TCP_FIN)) != 0)
    seq--;

  return ctl_pkt_enqueue(ctx, sock, sock->local_ip, sock->local_port, sock->remote_ip,
      sock->remote_port, seq, sock->rx_seq, flags, tcp_sock_rx_wnd(sock));
}

int tcp_ctl_tx_reply(struct tcp_slow_context *ctx, __u32 local_ip,
    __u16 local_port, __u32 remote_ip, __u16 remote_port, __u32 seq,
    __u32 ack, __u16 flags)
{
  return ctl_pkt_enqueue(ctx, NULL, local_ip, local_port, remote_ip, remote_port,
      seq, ack, flags, 0);
}

int tcp_tx_retransmit(struct tcp_slow_context *ctx, struct tcp_sock *sock)
{
  int ret;
  struct tcp_queue_ctl_entry *sig_qe;

  sig_qe = queue_tail(ctx->slow_fast_sig_q);
  if (sig_qe == NULL)
    return -1;

  sig_qe->data.ctl_remit.sock_id = sock->id;
  ret = queue_enqueue(ctx->slow_fast_sig_q, TCP_QUEUE_TX_RETRANSMIT);
  if (ret != 0)
    return -1;

  return 0;
}

/*** Ctrl TX Helpers **********************************************************/

static int ctl_pkt_enqueue(struct tcp_slow_context *ctx, const struct tcp_sock *sock,
    __u32 local_ip,
    __u16 local_port, __u32 remote_ip, __u16 remote_port, __u32 seq,
    __u32 ack, __u16 flags, __u16 wnd)
{
  int ret;
  struct tcp_queue_pkt_entry *pkt_qe;
  struct tcp_queue_ctl_entry *sig_qe;

  pkt_qe = queue_tail(ctx->slow_fast_pkt_q);
  if (pkt_qe == NULL)
    return -1;

  sig_qe = queue_tail(ctx->slow_fast_sig_q);
  if (sig_qe == NULL)
    return -1;

  ctl_pkt_fill(&pkt_qe->data.ctl_pkt, sock, local_ip, local_port, remote_ip,
      remote_port, seq, ack, flags, wnd);
  sig_qe->data.ctl_sig.ready = 1;

  ret = queue_enqueue(ctx->slow_fast_pkt_q, TCP_QUEUE_CTL_TX_PKT);
  if (ret != 0)
    return -1;

  MEM_BARRIER();
  ret = queue_enqueue(ctx->slow_fast_sig_q, TCP_QUEUE_CTL_TX);
  if (ret != 0)
    return -1;

  return 0;
}

static void ctl_pkt_fill(struct tcp_queue_ctl_pkt *pkt,
    const struct tcp_sock *sock, __u32 local_ip,
    __u16 local_port, __u32 remote_ip, __u16 remote_port, __u32 seq,
    __u32 ack, __u16 flags, __u16 wnd)
{
  __u32 now_us;

  IPH_VHL_SET(&pkt->pkt.ip, 4, 5);
  pkt->pkt.ip._tos = 0;
  pkt->pkt.ip.len = t_beui16(sizeof(pkt->pkt) + (sock != NULL ? TCP_TS_OPT_LEN : 0));
  pkt->pkt.ip.id = t_beui16(3);
  pkt->pkt.ip.offset = t_beui16(0);
  pkt->pkt.ip.ttl = 0xff;
  pkt->pkt.ip.proto = IP_PROTO_TCP;
  pkt->pkt.ip.src = t_beui32(local_ip);
  pkt->pkt.ip.dst = t_beui32(remote_ip);
  pkt->pkt.ip.chksum = 0;

  pkt->pkt.tcp.src = t_beui16(local_port);
  pkt->pkt.tcp.dest = t_beui16(remote_port);
  pkt->pkt.tcp.seqno = t_beui32(seq);
  pkt->pkt.tcp.ackno = t_beui32(ack);
  TCPH_HDRLEN_FLAGS_SET(&pkt->pkt.tcp,
      (TCP_HLEN + (sock != NULL ? TCP_TS_OPT_LEN : 0)) / 4, flags);
  pkt->pkt.tcp.wnd = t_beui16(wnd);
  pkt->pkt.tcp.chksum = 0;
  pkt->pkt.tcp.urgp = t_beui16(0);
  if (sock == NULL)
    return;

  now_us = (__u32) clock_tsc_to_us(clock_rdtsc());
  pkt->ts_opt.nop0 = TCP_OPT_NO_OP;
  pkt->ts_opt.nop1 = TCP_OPT_NO_OP;
  pkt->ts_opt.ts.kind = TCP_OPT_TIMESTAMP;
  pkt->ts_opt.ts.length = sizeof(pkt->ts_opt.ts);
  pkt->ts_opt.ts.ts_val = t_beui32(now_us);
  pkt->ts_opt.ts.ts_ecr = t_beui32(sock->ts_recent);
}

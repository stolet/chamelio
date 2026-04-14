#include "tcp_internal.h"
#include "queue_fns.h"
#include "tcp_hdr.h"

/*** Ctrl TX Helpers **********************************************************/

static int ctrl_pkt_enqueue(struct tcp_slow_context *ctx, __u32 local_ip,
    __u16 local_port, __u32 remote_ip, __u16 remote_port, __u32 seq,
    __u32 ack, __u16 flags, __u16 wnd);
static void ctrl_pkt_fill(struct tcp_pkt_inner *pkt, __u32 local_ip,
    __u16 local_port, __u32 remote_ip, __u16 remote_port, __u32 seq,
    __u32 ack, __u16 flags, __u16 wnd);

/*** Ctrl TX API **************************************************************/

int tcp_ctrl_tx(struct tcp_slow_context *ctx, struct tcp_sock *sock, __u16 flags)
{
  return ctrl_pkt_enqueue(ctx, sock->local_ip, sock->local_port, sock->remote_ip,
      sock->remote_port, sock->tx_seq, sock->rx_seq, flags,
      tcp_sock_rx_wnd(sock));
}

int tcp_ctrl_tx_resend(struct tcp_slow_context *ctx, struct tcp_sock *sock,
    __u16 flags)
{
  __u32 seq;

  seq = sock->tx_seq;
  if ((flags & (TAS_TCP_SYN | TAS_TCP_FIN)) != 0)
    seq--;

  return ctrl_pkt_enqueue(ctx, sock->local_ip, sock->local_port, sock->remote_ip,
      sock->remote_port, seq, sock->rx_seq, flags, tcp_sock_rx_wnd(sock));
}

int tcp_ctrl_tx_reply(struct tcp_slow_context *ctx, __u32 local_ip,
    __u16 local_port, __u32 remote_ip, __u16 remote_port, __u32 seq,
    __u32 ack, __u16 flags)
{
  return ctrl_pkt_enqueue(ctx, local_ip, local_port, remote_ip, remote_port,
      seq, ack, flags, 0);
}

int tcp_tx_retransmit(struct tcp_slow_context *ctx, struct tcp_sock *sock)
{
  int ret;
  struct tcp_queue_bump_entry *sig_qe;

  sig_qe = queue_tail(ctx->slow_fast_sig_q);
  if (sig_qe == NULL)
    return -1;

  sig_qe->data.fast_sock.sock_id = sock->id;
  ret = queue_enqueue(ctx->slow_fast_sig_q, TCP_QUEUE_TX_RETRANSMIT);
  if (ret != 0)
    return -1;

  return 0;
}

/*** Ctrl TX Helpers **********************************************************/

static int ctrl_pkt_enqueue(struct tcp_slow_context *ctx, __u32 local_ip,
    __u16 local_port, __u32 remote_ip, __u16 remote_port, __u32 seq,
    __u32 ack, __u16 flags, __u16 wnd)
{
  int ret;
  struct tcp_queue_bump_entry *pkt_qe;
  struct tcp_queue_bump_entry *sig_qe;

  pkt_qe = queue_tail(ctx->slow_fast_pkt_q);
  if (pkt_qe == NULL)
    return -1;

  sig_qe = queue_tail(ctx->slow_fast_sig_q);
  if (sig_qe == NULL)
    return -1;

  ctrl_pkt_fill(&pkt_qe->data.ctrl_pkt.pkt, local_ip, local_port, remote_ip,
      remote_port, seq, ack, flags, wnd);
  sig_qe->data.ctrl_sig.ready = 1;

  ret = queue_enqueue(ctx->slow_fast_pkt_q, TCP_QUEUE_CTRL_TX_PKT);
  if (ret != 0)
    return -1;

  MEM_BARRIER();
  ret = queue_enqueue(ctx->slow_fast_sig_q, TCP_QUEUE_CTRL_TX);
  if (ret != 0)
    return -1;

  return 0;
}

static void ctrl_pkt_fill(struct tcp_pkt_inner *pkt, __u32 local_ip,
    __u16 local_port, __u32 remote_ip, __u16 remote_port, __u32 seq,
    __u32 ack, __u16 flags, __u16 wnd)
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
  pkt->tcp.wnd = t_beui16(wnd);
  pkt->tcp.chksum = 0;
  pkt->tcp.urgp = t_beui16(0);
}

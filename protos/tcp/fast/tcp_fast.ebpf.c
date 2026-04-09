#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

#include "cham_fast.h"
#include "tcp_hdr.h"
#include "ip_hdr.h"
#include "tcp.h"
#include "tcp_queue_types.h"
#include "utils.h"

#define PORT_MAP 0
#define SOCK_MAP 1
#define FLOW_MAP 2
#define CFG_MAP 3
#define TCP_MAX_PAYLOAD (FAST_L3_PKT_ROOM - sizeof(struct tcp_pkt_inner))

static __always_inline struct tcp_sock *tcp_flow_find(struct cham_ebpf_ctx *ctx,
    __u32 local_ip, __u16 local_port, __u32 remote_ip, __u16 remote_port);
static __always_inline int punt_ctrl_rx(struct cham_ebpf_ctx *ctx,
    struct ip_hdr *ip, struct tcp_hdr *tcp);
static __always_inline int handle_bump_rx(struct cham_ebpf_ctx *ctx);
static __always_inline int handle_bump_tx(struct cham_ebpf_ctx *ctx);
static __always_inline int handle_ctrl_tx(struct cham_ebpf_ctx *ctx);
static __always_inline int handle_tx_command(struct cham_ebpf_ctx *ctx,
    int retransmit);
static __always_inline int tcp_emit_packet(struct cham_ebpf_ctx *ctx,
    struct tcp_sock *sock, __u16 *payload_len_out);
static __always_inline void fill_headers(struct tcp_pkt_inner *p,
    __u32 local_ip, __u16 local_port, __u32 remote_ip, __u16 remote_port,
    __u32 seq, __u32 ack, __u16 flags, __u16 wnd, __u16 payload_len);
static __always_inline __u32 tcp_tx_sched_avail(struct tcp_sock *sock);
static __always_inline __u16 tcp_rx_window(const struct tcp_sock *sock);
static __always_inline struct cham_sched_entry *tcp_sched_entry_lookup(
    struct cham_ebpf_ctx *ctx, __u32 sock_id);
static __always_inline int tcp_sched_entry_active(struct cham_ebpf_ctx *ctx,
    __u32 sock_id, __u64 *priority);
static __always_inline __u64 tcp_sched_ack_priority(
    struct cham_ebpf_ctx *ctx, const struct tcp_sock *sock);
static __always_inline __u64 tcp_sched_data_priority(
    struct cham_ebpf_ctx *ctx, const struct tcp_sock *sock);
static __always_inline __u64 tcp_sched_after_send_priority(
    struct cham_ebpf_ctx *ctx, __u64 cur_priority, __u16 payload_len,
    __u32 rate);
static __always_inline int schedule_sock_tx(struct cham_ebpf_ctx *ctx,
    struct tcp_sock *sock, __u32 old_avail);
static __always_inline __u32 flow_hash(__u32 local_ip, __u16 local_port,
    __u32 remote_ip, __u16 remote_port);
static __always_inline int tcp_seq_lt(__u32 a, __u32 b);
static __always_inline int tcp_seq_leq(__u32 a, __u32 b);
static __always_inline __u32 tcp_recovery_avail(const struct tcp_sock *sock,
    __u32 next_seq);
static __always_inline int tcp_recovery_rewind(struct tcp_sock *sock);
static __always_inline void tcp_recovery_start(struct tcp_sock *sock,
    __u32 end_seq);
static __always_inline void tcp_recovery_maybe_complete(struct tcp_sock *sock);

/* Add these functions as helpers */
static void * (*ebpf_queue_tail)(struct equeue *q, __u64 elsize) = (void *) 1001;
static int (*queue_enqueue)(struct equeue *q, __u8 type) = (void *) 1002;
static void * (*ebpf_queue_head)(struct dqueue *q, __u64 elsize) = (void *) 1012;
static int (*queue_dequeue)(struct dqueue *q) = (void *) 1013;

static void * (*ebpf_memcpy)(void *dst, void *src, size_t len) = (void *) 1003;
static void (*ebpf_print)(int a) = (void *) 1004;

static __u16 (*ebpf_ipv4_checksum)(void *ip_hdr) = (void *) 1005;
static __u16 (*ebpf_ipv4_udptcp_cksum)(void *ip_hdr, void *udp_hdr) = (void *) 1006;
static __u64 (*ebpf_rdtsc)(void) = (void *) 1014;
static __u64 (*ebpf_rate_delay_tsc)(__u32 bytes, __u32 rate_kbps) = (void *) 1015;
static struct cham_sched_entry * (*ebpf_sched_head)(struct cham_scheduler *sched, __u64 elsize) = (void *) 1007;
static int (*sched_pop)(struct cham_scheduler *sched) = (void *) 1008;
static int (*sched_add)(struct cham_scheduler *sched, __u32 id, __u64 priority,
    __u32 avail) = (void *) 1009;

static void * (*ebpf_map_get)(void *map_base, __u32 len) = (void *) 1010;
static void * (*ebpf_map_lookup)(void *map_base, __u64 id, __u64 elsize) = (void *) 1011;

static void (*ebpf_spin_lock)(volatile __u32 *) = (void *) 1016;
static void (*ebpf_spin_unlock)(volatile __u32 *) = (void *) 1017;

static __always_inline int tcp_seq_lt(__u32 a, __u32 b)
{
  return (__s32) (a - b) < 0;
}

static __always_inline int tcp_seq_leq(__u32 a, __u32 b)
{
  return (__s32) (a - b) <= 0;
}

static __always_inline __u32 tcp_recovery_avail(const struct tcp_sock *sock,
    __u32 next_seq)
{
  __u32 outstanding;

  if (sock->recovery_active == 0)
    return ~(__u32) 0;

  if (!tcp_seq_lt(next_seq, sock->recovery_end_seq))
    return 0;

  outstanding = sock->recovery_end_seq - next_seq;
  if (sock->tx_pending >= TCP_PAYLOAD_MSS)
    return 0;
  if (outstanding > TCP_PAYLOAD_MSS - sock->tx_pending)
    outstanding = TCP_PAYLOAD_MSS - sock->tx_pending;

  return outstanding;
}

static __always_inline int tcp_recovery_rewind(struct tcp_sock *sock)
{
  if (sock->recovery_active == 0)
  {
    if (sock->tx_pending == 0)
      return 0;

    tcp_recovery_start(sock, sock->tx_seq + sock->tx_pending);
  }
  else if (!tcp_seq_lt(sock->tx_seq, sock->recovery_end_seq))
  {
    tcp_recovery_maybe_complete(sock);
    return 0;
  }

  sock->tx_avail += sock->tx_pending;
  sock->tx_pending = 0;
  sock->tx_rexmit_seq = sock->tx_seq;
  sock->tx_rexmit_end_seq = sock->tx_seq;
  return 1;
}

static __always_inline void tcp_recovery_start(struct tcp_sock *sock,
    __u32 end_seq)
{
  sock->recovery_active = 1;
  sock->recovery_end_seq = end_seq;
}

static __always_inline void tcp_recovery_maybe_complete(struct tcp_sock *sock)
{
  if (sock->recovery_active == 0)
    return;

  if (tcp_seq_leq(sock->recovery_end_seq, sock->tx_seq))
  {
    sock->recovery_active = 0;
    sock->recovery_end_seq = 0;
  }
}


SEC("chamelio/event_rx")
int event_rx(struct cham_ebpf_ctx *ctx)
{
  int ret;
  __u16 ip_hdrs_len, ip_total_len, tcp_hdrs_len, payload_len, flags;
  __u64 now_tsc;
  struct ip_hdr *ip;
  struct tcp_hdr *tcp;
  void *payload;
  struct tcp_sock *sock;
  struct equeue *q;
  struct tcp_queue_bump_entry *qe;
  struct tcp_queue_bump_app_rx *bump;
  __u8 *rx_base;
  __u32 free_bytes, tail, part, ack_bump, seqno, overlap;
  __u32 ackno, old_tx_avail, new_head, old_remote_avail, new_remote_avail;
  __u32 snd_nxt, ack_pending;

  if (ctx->pkt + sizeof(struct ip_hdr) > ctx->pkt_end)
    return -1;

  ip = (struct ip_hdr *) ctx->pkt;
  if (IPH_V(ip) != 4 || IPH_HL(ip) < 5)
    return -1;

  if (ip->proto != IP_PROTO_TCP)
    return -1;

  if (f_beui16(ip->offset) & 0x3FFF)
    return -1;

  ip_hdrs_len = (__u16) (IPH_HL(ip) * 4);
  ip_total_len = f_beui16(ip->len);
  if (ip_total_len < ip_hdrs_len + (__u16) sizeof(struct tcp_hdr))
    return -1;

  if ((__u8 *) ip + ip_hdrs_len + sizeof(struct tcp_hdr) >
      (__u8 *) ctx->pkt_end)
    return -1;

  tcp = (struct tcp_hdr *) ((__u8 *) ip + ip_hdrs_len);
  tcp_hdrs_len = (__u16) (TCPH_HDRLEN(tcp) * 4);
  if (tcp_hdrs_len < sizeof(struct tcp_hdr))
    return -1;

  if (ip_total_len < ip_hdrs_len + tcp_hdrs_len)
    return -1;

  flags = TCPH_FLAGS(tcp);
  payload_len = ip_total_len - ip_hdrs_len - tcp_hdrs_len;
  seqno = f_beui32(tcp->seqno);
  ackno = f_beui32(tcp->ackno);
  
  sock = tcp_flow_find(ctx, f_beui32(ip->dst), f_beui16(tcp->dest),
      f_beui32(ip->src), f_beui16(tcp->src));

  if (sock == NULL && (((flags & TAS_TCP_SYN) == 0) || payload_len != 0 ||
      (flags & ~(TAS_TCP_SYN | TAS_TCP_ECE | TAS_TCP_CWR)) != 0))
    return -1;
  else if (sock == NULL)
    return punt_ctrl_rx(ctx, ip, tcp);

  ebpf_spin_lock(&sock->lock);
    
  if ((sock->state != TCP_SOCK_STATE_ESTABLISHED ) ||
      (flags & (TAS_TCP_SYN | TAS_TCP_FIN | TAS_TCP_RST)) != 0 ||
      (flags & ~(TAS_TCP_ACK | TAS_TCP_PSH | TAS_TCP_ECE | TAS_TCP_CWR)) != 0)
  {
    ret = punt_ctrl_rx(ctx, ip, tcp);
    ebpf_spin_unlock(&sock->lock);
    return ret;
  }

  now_tsc = ebpf_rdtsc();
  sock->rx_last_tsc = now_tsc;
  old_tx_avail = tcp_tx_sched_avail(sock);
  old_remote_avail = sock->tx_remote_avail;
  new_remote_avail = f_beui16(tcp->wnd);
  snd_nxt = sock->tx_seq + sock->tx_pending;
  if (sock->recovery_active != 0 && tcp_seq_lt(snd_nxt, sock->recovery_end_seq))
    snd_nxt = sock->recovery_end_seq;

  if ((flags & TAS_TCP_ACK) == 0)
  {
    sock->flags |= TCP_SOCK_FLAG_SEND_ACK;
    ret = schedule_sock_tx(ctx, sock, old_tx_avail);
    ebpf_spin_unlock(&sock->lock);
    return ret;
  }

  if (tcp_seq_lt(snd_nxt, ackno))
  {
    sock->flags |= TCP_SOCK_FLAG_SEND_ACK;
    ret = schedule_sock_tx(ctx, sock, old_tx_avail);
    ebpf_spin_unlock(&sock->lock);
    return ret;
  }

  sock->tx_remote_avail = new_remote_avail;
  sock->cc_acks++;
  ack_bump = 0;
  if (tcp_seq_leq(sock->tx_seq, ackno))
  {
    ack_bump = ackno - sock->tx_seq;
    if (ack_bump != 0)
    {
      sock->ack_advance_last_tsc = now_tsc;
      q = &ctx->equeues[sock->app_bump_qid].eq;
      qe = (struct tcp_queue_bump_entry *) ebpf_queue_tail(q,
          sizeof(struct tcp_queue_bump_entry));

      if (qe == NULL)
      {
        ebpf_spin_unlock(&sock->lock);
        return -1;
      }

      new_head = sock->tx_head + ack_bump;
      if (new_head >= sock->tx_len)
        new_head -= sock->tx_len;
      sock->tx_head = new_head;
      sock->tx_seq += ack_bump;
      ack_pending = ack_bump;
      if (ack_pending >= sock->tx_pending)
      {
        ack_pending -= sock->tx_pending;
        sock->tx_pending = 0;
        if (ack_pending > sock->tx_avail)
        {
          ebpf_spin_unlock(&sock->lock);
          return -1;
        }
        sock->tx_avail -= ack_pending;
      }
      else
      {
        sock->tx_pending -= ack_pending;
      }
      tcp_recovery_maybe_complete(sock);

      sock->rx_dupack_cnt = 0;
      sock->cc_ackb += ack_bump;
      if ((flags & TAS_TCP_ECE) != 0)
        sock->cc_ecnb += ack_bump;

      qe->data.bump_app_tx.opaque = sock->opaque;
      qe->data.bump_app_tx.tx_head = ack_bump;
      ret = queue_enqueue(q, TCP_QUEUE_BUMP_APP_TX);
      if (ret != 0)
      {
        ebpf_spin_unlock(&sock->lock);
        return -1;
      }
    }
    else if (sock->tx_pending != 0 &&
             ackno == sock->tx_seq &&
             (payload_len != 0 || new_remote_avail == old_remote_avail))
    {
      /* Count duplicate cumulative ACKs even when they are piggybacked
       * on incoming data. */
      sock->rx_dupack_cnt++;
      if (sock->rx_dupack_cnt >= 3)
      {
        if (tcp_recovery_rewind(sock))
        {
          sock->rx_dupack_cnt = 0;
          sock->cc_drops++;
        }
      }
    }
    else
    {
      sock->rx_dupack_cnt = 0;
    }
  }
  else
  {
    sock->rx_dupack_cnt = 0;
  }

  /* Return if this is a pure ACK */
  if (payload_len == 0)
  {
    ret = schedule_sock_tx(ctx, sock, old_tx_avail);
    ebpf_spin_unlock(&sock->lock);
    return ret;
  }

  if (IPH_ECN(ip) == CHAM_IP_ECN_CE)
    sock->flags |= TCP_SOCK_FLAG_SEND_ECE;

  if (tcp_seq_lt(sock->rx_seq, seqno))
  {
    sock->flags |= TCP_SOCK_FLAG_SEND_ACK;
    ret = schedule_sock_tx(ctx, sock, old_tx_avail);
    ebpf_spin_unlock(&sock->lock);
    return ret;
  }

  /* Schedule ACK for transmission if this is a duplicate packet */
  if (tcp_seq_leq(seqno + payload_len, sock->rx_seq))
  {
    sock->flags |= TCP_SOCK_FLAG_SEND_ACK;
    ret = schedule_sock_tx(ctx, sock, old_tx_avail);
    ebpf_spin_unlock(&sock->lock);
    return ret;
  }

  /* Trim duplicate prefix so only unseen bytes enter the RX ring. */
  payload = (void *) ((__u8 *) tcp + tcp_hdrs_len);
  if (tcp_seq_lt(seqno, sock->rx_seq))
  {
    overlap = sock->rx_seq - seqno;
    payload = (__u8 *) payload + overlap;
    payload_len -= overlap;
  }

  rx_base = ebpf_map_get(ctx->shm_base + sock->rx_off, sock->rx_len);
  free_bytes = sock->rx_len - sock->rx_avail;
  if (payload_len > free_bytes)
  {
    sock->flags |= TCP_SOCK_FLAG_SEND_ACK;
    ret = schedule_sock_tx(ctx, sock, old_tx_avail);
    ebpf_spin_unlock(&sock->lock);
    return ret;
  }

  q = &ctx->equeues[sock->app_bump_qid].eq;
  qe = ebpf_queue_tail(q, sizeof(struct tcp_queue_bump_entry));
  if (qe == NULL)
  {
    ebpf_spin_unlock(&sock->lock);
    return -1;
  }

  tail = sock->rx_head + sock->rx_avail;
  if (tail >= sock->rx_len)
    tail -= sock->rx_len;
  if (tail + payload_len <= sock->rx_len)
  {
    ebpf_memcpy(rx_base + tail, payload, payload_len);
  }
  else
  {
    part = sock->rx_len - tail;
    ebpf_memcpy(rx_base + tail, payload, part);
    ebpf_memcpy(rx_base, (__u8 *) payload + part, payload_len - part);
  }

  sock->rx_avail += payload_len;
  sock->rx_seq += payload_len;

  bump = &qe->data.bump_app_rx;
  bump->opaque = sock->opaque;
  bump->rx_avail = payload_len;
  bump->rx_port = f_beui16(tcp->src);
  bump->rx_ip = f_beui32(ip->src);

  ret = queue_enqueue(q, TCP_QUEUE_BUMP_APP_RX);
  if (ret != 0)
  {
    ebpf_spin_unlock(&sock->lock);
    return -1;
  }

  sock->flags |= TCP_SOCK_FLAG_SEND_ACK;
  ret = schedule_sock_tx(ctx, sock, old_tx_avail);
  ebpf_spin_unlock(&sock->lock);
  return ret;
}

SEC("chamelio/event_tx")
int event_tx(struct cham_ebpf_ctx *ctx)
{
  __u64 now_tsc;
  __u64 sched_priority;
  __u32 sock_id;
  struct tcp_sock *sock;
  struct cham_sched_entry *sched_entry;
  struct cham_map *map;
  __u16 payload_len;
  int ret;

  sched_entry = ebpf_sched_head(&ctx->sched,
      sizeof(struct cham_sched_entry));
  if (sched_entry == NULL)
    return -1;

  now_tsc = ebpf_rdtsc();
  if (sched_entry->priority > now_tsc)
    return -1;

  sched_priority = sched_entry->priority;
  sock_id = sched_entry->id;
  ret = sched_pop(&ctx->sched);
  if (ret != 0)
    return -1;

  map = &ctx->maps[SOCK_MAP];
  sock = ebpf_map_lookup(map->addr, sock_id, sizeof(struct tcp_sock));
  if (sock == NULL || sock->state != TCP_SOCK_STATE_ESTABLISHED)
    return -1;

  ebpf_spin_lock(&sock->lock);
  ret = tcp_emit_packet(ctx, sock, &payload_len);
  if (ret < 0)
  {
    ebpf_spin_unlock(&sock->lock);
    return -1;
  }

  if (payload_len != 0 && sock->cc_rate != 0 &&
      sched_priority > ctx->sched.vtime)
  {
    ctx->sched.vtime = sched_priority;
  }

  if (tcp_tx_sched_avail(sock) != 0)
  {
    __u64 next_priority;

    if (sock->cc_rate == 0)
      next_priority = ebpf_rdtsc();
    else
      next_priority = tcp_sched_after_send_priority(ctx, sched_priority,
          payload_len, sock->cc_rate);

    ret = sched_add(&ctx->sched, sock->id, next_priority,
        tcp_tx_sched_avail(sock));
    if (ret != 0)
    {
      ebpf_spin_unlock(&sock->lock);
      return -1;
    }
  }

  ebpf_spin_unlock(&sock->lock);
  return ret;
}

SEC("chamelio/event_deq")
int event_deq(struct cham_ebpf_ctx *ctx)
{
  if ((__u8 *) ctx->qe + sizeof(struct tcp_queue_bump_entry) >
      (__u8 *) ctx->shm_end)
    return -1;

  switch (ctx->qe->type)
  {
    case TCP_QUEUE_BUMP_CHAM_TX:
      return handle_bump_tx(ctx);
    case TCP_QUEUE_BUMP_CHAM_RX:
      return handle_bump_rx(ctx);
    case TCP_QUEUE_CTRL_TX:
      return handle_ctrl_tx(ctx);
    case TCP_QUEUE_TX_SCHED:
      return handle_tx_command(ctx, 0);
    case TCP_QUEUE_TX_RETRANSMIT:
      return handle_tx_command(ctx, 1);
    default:
      return -1;
  }
}

static __always_inline int handle_bump_tx(struct cham_ebpf_ctx *ctx)
{
  __u32 tx_ip;
  __u32 old_tx_avail;
  __u16 tx_port;
  struct tcp_sock *sock;
  struct tcp_queue_bump_entry *qe;
  struct tcp_queue_bump_cham_tx *bump_cham;
  struct cham_map *map;

  qe = (struct tcp_queue_bump_entry *) ctx->qe;
  bump_cham = &qe->data.bump_cham_tx;

  map = &ctx->maps[SOCK_MAP];
  sock = ebpf_map_lookup(map->addr, bump_cham->sock_id, sizeof(struct tcp_sock));
  if (sock == NULL || sock->state != TCP_SOCK_STATE_ESTABLISHED)
    return -1;

  ebpf_spin_lock(&sock->lock);
  tx_ip = bump_cham->tx_ip == 0 ? sock->remote_ip : bump_cham->tx_ip;
  tx_port = bump_cham->tx_port == 0 ? sock->remote_port : bump_cham->tx_port;
  if (tx_port == 0 || tx_ip == 0)
  {
    ebpf_spin_unlock(&sock->lock);
    return -1;
  }
  if (bump_cham->tx_avail == 0 ||
      bump_cham->tx_avail > sock->tx_len - sock->tx_avail - sock->tx_pending)
  {
    ebpf_spin_unlock(&sock->lock);
    return -1;
  }

  old_tx_avail = tcp_tx_sched_avail(sock);
  sock->remote_ip = tx_ip;
  sock->remote_port = tx_port;
  sock->tx_avail += bump_cham->tx_avail;

  if (schedule_sock_tx(ctx, sock, old_tx_avail) != 0)
  {
    ebpf_spin_unlock(&sock->lock);
    return -1;
  }
  
  ebpf_spin_unlock(&sock->lock);
  return 0;
}

static __always_inline int handle_bump_rx(struct cham_ebpf_ctx *ctx)
{
  __u16 old_wnd, new_wnd;
  __u32 new_head;
  struct tcp_sock *sock;
  struct tcp_queue_bump_cham_rx *bump;
  struct tcp_queue_bump_entry *qe;
  struct cham_map *map;

  qe = (struct tcp_queue_bump_entry *) ctx->qe;
  bump = &qe->data.bump_cham_rx;

  map = &ctx->maps[SOCK_MAP];
  sock = ebpf_map_lookup(map->addr, bump->sock_id, sizeof(struct tcp_sock));
  if (sock == NULL || bump->rx_head == 0 || bump->rx_head > sock->rx_avail)
    return -1;

  ebpf_spin_lock(&sock->lock);
  old_wnd = tcp_rx_window(sock);
  new_head = sock->rx_head + bump->rx_head;
  if (new_head >= sock->rx_len)
    new_head -= sock->rx_len;
  sock->rx_head = new_head;
  sock->rx_avail -= bump->rx_head;
  new_wnd = tcp_rx_window(sock);

  if (new_wnd > old_wnd)
  {
    sock->flags |= TCP_SOCK_FLAG_SEND_ACK;
    if (sched_add(&ctx->sched, sock->id, 
        tcp_sched_ack_priority(ctx, sock), 0) != 0)
    {
      ebpf_spin_unlock(&sock->lock);
      return -1;
    }
    
    ebpf_spin_unlock(&sock->lock);
    return 0;
  }

  ebpf_spin_unlock(&sock->lock);
  return 0;
}

static __always_inline __u32 tcp_tx_sched_avail(struct tcp_sock *sock)
{
  __u32 fc_avail;
  __u32 avail;
  __u32 next_seq;
  __u32 recovery_avail;

  if (sock->tx_pending >= sock->tx_remote_avail)
    return 0;

  fc_avail = sock->tx_remote_avail - sock->tx_pending;
  avail = sock->tx_avail < fc_avail ? sock->tx_avail : fc_avail;
  next_seq = sock->tx_seq + sock->tx_pending;
  recovery_avail = tcp_recovery_avail(sock, next_seq);
  if (avail > recovery_avail)
    avail = recovery_avail;
  return avail;
}

static __always_inline __u16 tcp_rx_window(const struct tcp_sock *sock)
{
  __u32 wnd;

  wnd = sock->rx_len - sock->rx_avail;
  if (wnd > 65535)
    wnd = 65535;

  return (__u16) wnd;
}

static __always_inline struct cham_sched_entry *tcp_sched_entry_lookup(
    struct cham_ebpf_ctx *ctx, __u32 sock_id)
{
  return ebpf_map_lookup(ctx->sched.entries, sock_id,
      sizeof(struct cham_sched_entry));
}

static __always_inline int tcp_sched_entry_active(struct cham_ebpf_ctx *ctx,
    __u32 sock_id, __u64 *priority)
{
  struct cham_sched_entry *entry;

  entry = tcp_sched_entry_lookup(ctx, sock_id);
  if (entry == NULL || entry->id == SCHED_ID_INVALID)
    return 0;

  if (priority != NULL)
    *priority = entry->priority;
  return 1;
}

static __always_inline __u64 tcp_sched_ack_priority(
    struct cham_ebpf_ctx *ctx, const struct tcp_sock *sock)
{
  __u64 priority;

  if (tcp_sched_entry_active(ctx, sock->id, &priority))
    return priority;

  return ebpf_rdtsc();
}

static __always_inline __u64 tcp_sched_data_priority(
    struct cham_ebpf_ctx *ctx, const struct tcp_sock *sock)
{
  __u64 now_tsc;
  __u64 priority;

  if (tcp_sched_entry_active(ctx, sock->id, &priority))
    return priority;

  now_tsc = ebpf_rdtsc();
  if (sock->cc_rate == 0)
    return now_tsc;

  return ctx->sched.vtime > now_tsc ? ctx->sched.vtime : now_tsc;
}

static __always_inline __u64 tcp_sched_after_send_priority(
    struct cham_ebpf_ctx *ctx, __u64 cur_priority, __u16 payload_len,
    __u32 rate)
{
  __u64 base;

  if (rate == 0 || payload_len == 0)
    return ebpf_rdtsc();

  base = ctx->sched.vtime > cur_priority ? ctx->sched.vtime : cur_priority;
  ctx->sched.vtime = base;
  return base + ebpf_rate_delay_tsc(payload_len, rate);
}

static __always_inline int schedule_sock_tx(struct cham_ebpf_ctx *ctx,
    struct tcp_sock *sock, __u32 old_avail)
{
  __u32 new_avail, sched_avail;
  __u64 sched_priority;

  new_avail = tcp_tx_sched_avail(sock);
  if (new_avail > old_avail)
  {
    sched_avail = new_avail - old_avail;
    sched_priority = tcp_sched_data_priority(ctx, sock);
  }
  else if ((sock->flags & TCP_SOCK_FLAG_SEND_ACK) != 0)
  {
    sched_avail = 0;
    sched_priority = tcp_sched_ack_priority(ctx, sock);
  }
  else
    return 0;

  if (sched_add(&ctx->sched, sock->id, sched_priority, sched_avail) != 0)
    return -1;
  return 0;
}

static __always_inline int handle_ctrl_tx(struct cham_ebpf_ctx *ctx)
{
  int ret;
  __u16 pkt_len;
  struct cham_map *map;
  struct tcp_ctrl_cfg *cfg;
  struct dqueue *pkt_q;
  struct tcp_queue_bump_entry *pkt_qe;
  struct tcp_pkt_inner *pkt;
  struct tcp_queue_bump_entry *qe;
  struct tcp_pkt_inner *p = (struct tcp_pkt_inner *) ctx->pkt;

  if ((__u8 *) p + sizeof(struct tcp_pkt_inner) > (__u8 *) ctx->pkt_end)
    return -1;

  qe = (struct tcp_queue_bump_entry *) ctx->qe;
  if (qe->type != TCP_QUEUE_CTRL_TX)
    return -1;

  map = &ctx->maps[CFG_MAP];
  cfg = ebpf_map_lookup(map->addr, 0, sizeof(struct tcp_ctrl_cfg));
  if (cfg == NULL || ctx->dqueues == NULL)
    return -1;

  pkt_q = &ctx->dqueues[cfg->slow_fast_pkt_qid].dq;
  pkt_qe = ebpf_queue_head(pkt_q, sizeof(struct tcp_queue_bump_entry));
  if (pkt_qe == NULL)
    return -1;

  if ((__u8 *) pkt_qe + sizeof(struct tcp_queue_bump_entry) >
      (__u8 *) ctx->shm_end)
  {
    return -1;
  }
  if (pkt_qe->type != TCP_QUEUE_CTRL_TX_PKT)
    return -1;

  pkt = &pkt_qe->data.ctrl_pkt.pkt;
  pkt_len = f_beui16(pkt->ip.len);
  if (pkt_len < sizeof(struct tcp_pkt_inner))
    return -1;
  if ((__u8 *) p + pkt_len > (__u8 *) ctx->pkt_end)
    return -1;

  ebpf_memcpy(p, pkt, sizeof(*pkt));
  p->ip.chksum = 0;
  p->tcp.chksum = 0;
  p->ip.chksum = ebpf_ipv4_checksum(&p->ip);
  p->tcp.chksum = ebpf_ipv4_udptcp_cksum(&p->ip, &p->tcp);
  
  ret = queue_dequeue(pkt_q);
  if (ret != 0)
    return -1;

  return pkt_len;
}

static __always_inline int handle_tx_command(struct cham_ebpf_ctx *ctx,
    int retransmit)
{
  int ret;
  int had_sched_entry;
  struct cham_map *map;
  struct tcp_sock *sock;
  struct tcp_queue_bump_entry *qe;
  struct tcp_queue_fast_sock *cmd;
  __u32 avail;
  __u64 sched_priority;
  __u16 payload_len;

  qe = (struct tcp_queue_bump_entry *) ctx->qe;
  cmd = &qe->data.fast_sock;

  map = &ctx->maps[SOCK_MAP];
  sock = ebpf_map_lookup(map->addr, cmd->sock_id, sizeof(struct tcp_sock));
  if (sock == NULL || sock->state != TCP_SOCK_STATE_ESTABLISHED)
    return -1;

  ebpf_spin_lock(&sock->lock);
  had_sched_entry = tcp_sched_entry_active(ctx, sock->id, NULL);
  if (retransmit)
  {
    if (!tcp_recovery_rewind(sock))
    {
      ebpf_spin_unlock(&sock->lock);
      return 0;
    }
    sock->rx_dupack_cnt = 0;
  }

  if (retransmit)
  {
    ret = tcp_emit_packet(ctx, sock, &payload_len);
    if (ret < 0)
    {
      ebpf_spin_unlock(&sock->lock);
      return ret;
    }

    if (!had_sched_entry)
    {
      if (tcp_tx_sched_avail(sock) != 0)
      {
        if (sock->cc_rate == 0)
        {
          sched_priority = ebpf_rdtsc();
        }
        else
        {
          __u64 base = tcp_sched_data_priority(ctx, sock);
          sched_priority = base + ebpf_rate_delay_tsc(payload_len, sock->cc_rate);
        }

        if (sched_add(&ctx->sched, sock->id, sched_priority,
                tcp_tx_sched_avail(sock)) != 0)
        {
          ebpf_spin_unlock(&sock->lock);
          return -1;
        }
      }
    }

    ebpf_spin_unlock(&sock->lock);
    return ret;
  }

  avail = tcp_tx_sched_avail(sock);
  if (avail == 0 && (sock->flags & TCP_SOCK_FLAG_SEND_ACK) == 0)
  {
    ebpf_spin_unlock(&sock->lock);
    return 0;
  }

  if (avail == 0)
    sched_priority = tcp_sched_ack_priority(ctx, sock);
  else
    sched_priority = tcp_sched_data_priority(ctx, sock);

  if (sched_add(&ctx->sched, sock->id, sched_priority, avail) != 0)
  {
    ebpf_spin_unlock(&sock->lock);
    return -1;
  }
  
  ebpf_spin_unlock(&sock->lock);
  return 0;
}

static __always_inline int tcp_emit_packet(struct cham_ebpf_ctx *ctx,
    struct tcp_sock *sock, __u16 *payload_len_out)
{
  __u32 tx_ip;
  __u32 max_payload;
  __u32 tx_pos;
  __u32 tx_seq;
  __u64 part;
  __u8 tx_flags;
  struct tcp_pkt_inner *p = (struct tcp_pkt_inner *) ctx->pkt;
  __u16 payload_len, pkt_hdrs_len;

  if ((__u8 *) p + sizeof(struct tcp_pkt_inner) > (__u8 *) ctx->pkt_end)
    return -1;

  if (sock->remote_ip == 0 || sock->remote_port == 0)
    return -1;

  payload_len = tcp_tx_sched_avail(sock);

  max_payload = (__u32) ((__u8 *) ctx->pkt_end -
      ((__u8 *) p + sizeof(struct tcp_pkt_inner)));
  if (payload_len > TCP_MAX_PAYLOAD)
    payload_len = TCP_MAX_PAYLOAD;
  if (payload_len > max_payload)
    payload_len = max_payload;
  if (payload_len == 0 && (sock->flags & TCP_SOCK_FLAG_SEND_ACK) == 0)
    return -1;

  if ((__u8 *) p + sizeof(struct tcp_pkt_inner) + payload_len >
      (__u8 *) ctx->pkt_end)
    return -1;

  tx_ip = sock->remote_ip;
  tx_flags = TAS_TCP_ACK;
  if (payload_len != 0)
    tx_flags |= TAS_TCP_PSH;
  if ((sock->flags & TCP_SOCK_FLAG_SEND_ECE) != 0)
    tx_flags |= TAS_TCP_ECE;
  tx_seq = sock->tx_seq + sock->tx_pending;
  fill_headers(p, sock->local_ip, sock->local_port, tx_ip, sock->remote_port,
      tx_seq, sock->rx_seq, tx_flags,
      tcp_rx_window(sock), payload_len);
  if ((sock->flags & TCP_SOCK_FLAG_ECN) != 0 && payload_len != 0)
    IPH_ECN_SET(&p->ip, CHAM_IP_ECN_ECT0);
  pkt_hdrs_len = sizeof(struct ip_hdr) + TCP_HLEN;

  if (payload_len != 0)
  {
    tx_pos = sock->tx_head + sock->tx_pending;
    if (tx_pos >= sock->tx_len)
      tx_pos -= sock->tx_len;

    if (tx_pos + payload_len <= sock->tx_len)
    {
      ebpf_memcpy(ctx->pkt + pkt_hdrs_len, ctx->shm_base + sock->tx_off +
          tx_pos, payload_len);
    }
    else
    {
      part = sock->tx_len - tx_pos;
      ebpf_memcpy(ctx->pkt + pkt_hdrs_len, ctx->shm_base + sock->tx_off +
          tx_pos, part);
      ebpf_memcpy(ctx->pkt + pkt_hdrs_len + part, ctx->shm_base + sock->tx_off,
          payload_len - part);
    }

    sock->tx_avail -= payload_len;
    sock->tx_pending += payload_len;
  }

  sock->flags &= ~TCP_SOCK_FLAG_SEND_ACK;
  sock->flags &= ~TCP_SOCK_FLAG_SEND_ECE;

  p->ip.chksum = 0;
  p->tcp.chksum = 0;
  p->ip.chksum = ebpf_ipv4_checksum(&p->ip);
  p->tcp.chksum = ebpf_ipv4_udptcp_cksum(&p->ip, &p->tcp);
  
  *payload_len_out = payload_len;
  return pkt_hdrs_len + payload_len;
}

static __always_inline struct tcp_sock *tcp_flow_find(struct cham_ebpf_ctx *ctx,
    __u32 local_ip, __u16 local_port, __u32 remote_ip, __u16 remote_port)
{
  int i;
  __u32 sid;
  __u32 hash;
  struct tcp_flow_bucket *bucket;
  struct tcp_sock *sock;
  struct cham_map *map;

  hash = flow_hash(local_ip, local_port, remote_ip, remote_port)
      % TCP_FLOW_BUCKETS;

  map = &ctx->maps[FLOW_MAP];
  bucket = ebpf_map_lookup(map->addr, hash, sizeof(struct tcp_flow_bucket));
  if (bucket == NULL)
    return NULL;

  map = &ctx->maps[SOCK_MAP];
  for (i = 0; i < TCP_FLOW_BUCKET_SLOTS; i++)
  {
    sid = bucket->sids[i];
    if (sid == ID_INVALID)
      continue;
    sock = ebpf_map_lookup(map->addr, sid, sizeof(struct tcp_sock));
    if (sock == NULL)
      continue;
    if (sock->state == TCP_SOCK_STATE_CLOSED)
      continue;
    if (sock->local_ip == local_ip && sock->local_port == local_port &&
        sock->remote_ip == remote_ip && sock->remote_port == remote_port)
    {
      return sock;
    }
  }

  return NULL;
}

static __always_inline int punt_ctrl_rx(struct cham_ebpf_ctx *ctx,
    struct ip_hdr *ip, struct tcp_hdr *tcp)
{
  int ret;
  struct equeue *sig_q;
  struct equeue *pkt_q;
  struct tcp_ctrl_cfg *cfg;
  struct tcp_queue_bump_entry *sig_qe;
  struct tcp_queue_bump_entry *pkt_qe;
  struct cham_map *map;

  map = &ctx->maps[CFG_MAP];
  cfg = ebpf_map_lookup(map->addr, 0, sizeof(struct tcp_ctrl_cfg));
  if (cfg == NULL)
    return -1;

  pkt_q = &ctx->equeues[cfg->fast_slow_pkt_qid].eq;
  pkt_qe = ebpf_queue_tail(pkt_q, sizeof(struct tcp_queue_bump_entry));
  if (pkt_qe == NULL)
    return -1;

  sig_q = &ctx->equeues[cfg->fast_slow_sig_qid].eq;
  sig_qe = ebpf_queue_tail(sig_q, sizeof(struct tcp_queue_bump_entry));
  if (sig_qe == NULL)
    return -1;

  ebpf_memcpy(&pkt_qe->data.ctrl_pkt.pkt.ip, ip, sizeof(*ip));
  ebpf_memcpy(&pkt_qe->data.ctrl_pkt.pkt.tcp, tcp, sizeof(*tcp));
  sig_qe->data.ctrl_sig.ready = 1;

  ret = queue_enqueue(pkt_q, TCP_QUEUE_CTRL_RX_PKT);
  if (ret != 0)
    return -1;

  ret = queue_enqueue(sig_q, TCP_QUEUE_CTRL_RX);
  if (ret != 0)
    return -1;

  return 0;
}

static __always_inline void fill_headers(struct tcp_pkt_inner *p,
    __u32 local_ip, __u16 local_port, __u32 remote_ip, __u16 remote_port,
    __u32 seq, __u32 ack, __u16 flags, __u16 wnd, __u16 payload_len)
{
  IPH_VHL_SET(&p->ip, 4, 5);
  p->ip._tos = 0;
  p->ip.len = t_beui16(sizeof(struct ip_hdr) + TCP_HLEN + payload_len);
  p->ip.id = t_beui16(3);
  p->ip.offset = t_beui16(0);
  p->ip.ttl = 0xff;
  p->ip.proto = IP_PROTO_TCP;
  p->ip.src = t_beui32(local_ip);
  p->ip.dst = t_beui32(remote_ip);
  p->ip.chksum = 0;

  p->tcp.src = t_beui16(local_port);
  p->tcp.dest = t_beui16(remote_port);
  p->tcp.seqno = t_beui32(seq);
  p->tcp.ackno = t_beui32(ack);
  TCPH_HDRLEN_FLAGS_SET(&p->tcp, TCP_HLEN / 4, flags);
  p->tcp.wnd = t_beui16(wnd);
  p->tcp.chksum = 0;
  p->tcp.urgp = t_beui16(0);
}

static __always_inline __u32 flow_hash(__u32 local_ip, __u16 local_port,
    __u32 remote_ip, __u16 remote_port)
{
  __u32 h;

  h = local_ip ^ remote_ip ^ ((__u32) local_port << 16) ^ remote_port;
  return h ^ (h >> 16);
}
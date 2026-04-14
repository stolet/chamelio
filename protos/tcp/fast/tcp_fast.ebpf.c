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
#define SID_BUCK_MAP 2
#define CFG_MAP 3
#define TCP_MAX_PAYLOAD (FAST_L3_PKT_ROOM - sizeof(struct tcp_pkt_inner))
#define FAST_REMIT_THRESH 3

/* Miscelaneous helpers */
static __always_inline __u8 is_qe_valid(struct queue_entry *qe, void *shm_end);  
static __always_inline __u8 is_pktlen_valid(void *pkt, void *pkt_end);
static __always_inline __u8 is_paylen_valid(void *pkt, 
    void *pkt_end, __u16 hdrs_len, __u16 paylen);
static __always_inline void remit(struct cham_scheduler *sched,
    struct tcp_sock *sock);
  
/* RX helpers */
static __always_inline __u32 rx_get_hdrlen(struct tcp_pkt_inner *tcp_pkt);
static __always_inline __u32 rx_get_paylen(struct tcp_pkt_inner *tcp_pkt);
static __always_inline __u8 rx_is_ip(struct tcp_pkt_inner *tcp_pkt);
static __always_inline __u8 rx_is_flags_ctl(struct tcp_pkt_inner *tcp_pkt);
static __always_inline __u8 rx_is_ack(struct tcp_pkt_inner *tcp_pkt);
static __always_inline __u8 rx_is_ack_only(struct tcp_pkt_inner *tcp_pkt);
static __always_inline __u8 rx_is_payload_dup(struct tcp_pkt_inner *tcp_pkt,
    struct tcp_sock *sock);
static __always_inline __u8 rx_is_out_of_order(struct tcp_pkt_inner *tcp_pkt,
    struct tcp_sock *sock);
static __always_inline __u8 rx_is_ack_valid(__u32 seqno, 
    __u32 pending, __u32 ackno);
static __always_inline int rx_punt_ctl(struct cham_ebpf_ctx *ctx,
    struct tcp_pkt_inner *tcp_pkt);
static __always_inline __u32 rx_get_ack_bump(struct tcp_pkt_inner *tcp_pkt,
    struct tcp_sock *sock);
static __always_inline __u32 rx_get_rx_bump(struct tcp_sock *sock, __u32 paylen);
static __always_inline void rx_sock_bump_ack(struct tcp_sock *sock, 
    __u32 ack_bump);
static __always_inline void rx_sock_bump_rx(struct tcp_sock *sock, 
    __u32 rx_bump);
static __always_inline void rx_sched_ack(struct cham_scheduler *sched,
    struct tcp_sock *sock);
static __always_inline __u32 rx_get_payload_overlap(struct tcp_pkt_inner *tcp_pkt,
    struct tcp_sock *sock);
static __always_inline void rx_copy_payload(void *shm_base,
    struct tcp_sock *sock, void *payload, __u32 paylen);
static __always_inline int rx_enqueue_rx_bump(struct equeue *q,
    struct tcp_sock *sock, __u32 rx_bump, __u16 rx_port, __u32 rx_ip);
static __always_inline int rx_enqueue_tx_bump(struct equeue *q,
    struct tcp_sock *sock, __u32 tx_bump);

/* TX helpers */
static __always_inline __u32 tx_get_hdrlen();
static __always_inline __u32 tx_get_max_paylen(struct cham_ebpf_ctx *ctx,
    __u32 hdrlen);
static __always_inline __u32 tx_get_tx_bump(struct tcp_sock *sock,
    __u32 max_paylen);
static __always_inline void tx_fill_hdr(struct tcp_pkt_inner *tcp_pkt,
    struct tcp_sock *sock, __u32 hdrlen, __u32 paylen);
static __always_inline void tx_copy_payload(void *shm_base, struct tcp_sock *sock,
    void *payload, __u32 tx_bump);
static __always_inline void tx_sock_tx_bump(struct tcp_sock *sock,
    __u32 tx_bump);

/* DEQ helpers */
static __always_inline int deq_handle_bump_tx(struct cham_ebpf_ctx *ctx);
static __always_inline int deq_handle_bump_rx(struct cham_ebpf_ctx *ctx);
static __always_inline int deq_handle_ctl_tx(struct cham_ebpf_ctx *ctx);
static __always_inline int deq_handle_retransmit(struct cham_ebpf_ctx *ctx);
static __always_inline __u32 deq_get_tx_ip(struct tcp_sock *sock,
    struct tcp_queue_bump_cham_tx *bump);
static __always_inline __u32 deq_get_tx_port(struct tcp_sock *sock,
    struct tcp_queue_bump_cham_tx *bump);
static __always_inline __u32 deq_sock_bump_tx(struct tcp_sock *sock,
    struct tcp_queue_bump_cham_tx *bump);
static __always_inline __u32 deq_sock_bump_rx(struct tcp_sock *sock,
    struct tcp_queue_bump_cham_rx *bump);
    
/* Socket helpers */
static __always_inline struct tcp_sock * sock_find(struct cham_ebpf_ctx *ctx,
    struct tcp_pkt_inner *tcp_pkt);
static __always_inline __u32 sock_hash(__u32 lip, __u16 lport,
    __u32 rip, __u16 rport);
static __always_inline __u32 sock_sched_avail(struct tcp_sock *sock);

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
static int (*sched_remove)(struct cham_scheduler *sched, __u32 id) = (void *) 1018;

static void * (*ebpf_map_get)(void *map_base, __u32 len) = (void *) 1010;
static void * (*ebpf_map_lookup)(void *map_base, __u64 id, __u64 elsize) = (void *) 1011;

static void (*ebpf_spin_lock)(volatile __u32 *) = (void *) 1016;
static void (*ebpf_spin_unlock)(volatile __u32 *) = (void *) 1017;

SEC("chamelio/event_rx")
int event_rx(struct cham_ebpf_ctx *ctx)
{
  int ret;
  __u8 is_control, is_ack, is_ack_only, is_ack_dup, is_ip, is_out_of_order;
  __u8 is_payload_dup;
  __u8 should_fast_remit;
  __u8 pktlen_valid, paylen_valid;
  __u16 paylen, hdrlen;
  __u32 ack_bump, overlap, rx_bump;
  struct tcp_sock *sock;
  struct tcp_pkt_inner *tcp_pkt;
  void *payload;
  
  /* Do pkt len check to make ebpf verifier happy */
  pktlen_valid = is_pktlen_valid(ctx->pkt, ctx->pkt_end);
  if (!pktlen_valid)
    return -1;
  tcp_pkt = (struct tcp_pkt_inner *) ctx->pkt;

  is_ip = rx_is_ip(tcp_pkt);
  if (!is_ip)
  {
    return -1;
  }
  
  /* Send control packets to slow path */
  is_control = rx_is_flags_ctl(tcp_pkt);
  if (is_control)
  {
    ret = rx_punt_ctl(ctx, tcp_pkt);
    return ret;
  }
  
  /* Get socket and drop packet if we can't find a socket */
  sock = sock_find(ctx, tcp_pkt);
  if (sock == NULL)
    return -1;

  if (sock->state != TCP_SOCK_STATE_ESTABLISHED)
    return rx_punt_ctl(ctx, tcp_pkt);
  
  /* Process control window and update remote available */
  ebpf_spin_lock(&sock->lock);
  sock->tx_remote_avail = f_beui16(tcp_pkt->tcp.wnd);

  /* Get current state before processing ack */
  is_ack = rx_is_ack(tcp_pkt);
  is_ack_dup = sock->tx_seq == f_beui32(tcp_pkt->tcp.ackno);
  is_payload_dup = rx_is_payload_dup(tcp_pkt, sock);
  is_out_of_order = rx_is_out_of_order(tcp_pkt, sock);
  overlap = rx_get_payload_overlap(tcp_pkt, sock);
  
  /* Process ACK bump */
  ack_bump = rx_get_ack_bump(tcp_pkt, sock);
  if (is_ack)
    rx_sock_bump_ack(sock, ack_bump);
  
  /* Enqueue app bump if this was not a duplicate ACK */
  if (!is_ack_dup)
  {
    ret = rx_enqueue_tx_bump(&ctx->equeues[sock->app_bump_qid].eq, 
        sock, ack_bump);
    if (ret != 0)
    {
      ebpf_spin_unlock(&sock->lock);
      return -1;
    }
  }
  
  /* If ack is duplicate check if we should retransmit */
  should_fast_remit = 0;
  if (is_ack && is_ack_dup)
  {
    sock->rx_dupack_cnt++;
    should_fast_remit = sock->rx_dupack_cnt > FAST_REMIT_THRESH;
    if (should_fast_remit)
    {
      remit(&ctx->sched, sock);
      sock->rx_dupack_cnt = 0;
    }
  }
  
  /* ACK-only packets should not trigger a response. */
  is_ack_only = rx_is_ack_only(tcp_pkt);
  if (is_ack_only)
  {
    ebpf_spin_unlock(&sock->lock);
    return 0;
  }

  /* Fully duplicate payload still needs an ACK */
  if (is_payload_dup && !should_fast_remit)
  {
    rx_sched_ack(&ctx->sched, sock);
    ebpf_spin_unlock(&sock->lock);
    return 0;
  }
  
  /* If out of order schedule a pure ack and return */
  if (is_out_of_order)
  {
    rx_sched_ack(&ctx->sched, sock);
    ebpf_spin_unlock(&sock->lock);
    return 0;
  }
  
  /* Check if payload len valid so ebpf verifier is happy */
  hdrlen = rx_get_hdrlen(tcp_pkt);
  paylen = rx_get_paylen(tcp_pkt);
  paylen_valid = is_paylen_valid(ctx->pkt, ctx->pkt_end, hdrlen, paylen);
  if (!paylen_valid)
  {
    /* Trigger ACK and return if check failed */
    rx_sched_ack(&ctx->sched, sock);
    ebpf_spin_unlock(&sock->lock);
    return -1;
  }
  
  /* Trim duplicate prefix so only unseen bytes enter RX ring */
  payload = ctx->pkt + hdrlen + overlap;
  paylen -= overlap;
  
  /* Calculate bump for rx ring */
  rx_bump = rx_get_rx_bump(sock, paylen);
  
  /* Copy payload to rx buf and enqueue bump */
  if (rx_bump > 0)
  {
    rx_copy_payload(ctx->shm_base, sock, payload, rx_bump);
    rx_sock_bump_rx(sock, rx_bump);
    ret = rx_enqueue_rx_bump(&ctx->equeues[sock->app_bump_qid].eq, sock, rx_bump,
        f_beui16(tcp_pkt->tcp.src), f_beui32(tcp_pkt->ip.src));
    if (ret != 0)
    {
      ebpf_spin_unlock(&sock->lock);
      return -1;
    }
  }
  
  /* Schedule ACK */
  rx_sched_ack(&ctx->sched, sock);
  ebpf_spin_unlock(&sock->lock);
  
  return 0;
}

SEC("chamelio/event_tx")
int event_tx(struct cham_ebpf_ctx *ctx)
{
  int ret;
  __u8 pktlen_valid, paylen_valid;
  __u32 hdrlen, max_paylen, tx_bump, avail, sid;
  void *payload;
  struct cham_map *map;
  struct tcp_sock *sock;
  struct tcp_pkt_inner *tcp_pkt;
  struct cham_sched_entry *sched_entry;
  
  /* Check if scheduler is empty */
  sched_entry = ebpf_sched_head(&ctx->sched, sizeof(struct cham_sched_entry));
  if (sched_entry == NULL)
    return -1;
  
  /* TODO: Skip entry and return if it should be scheduled in future (used with cc) */
  
  /* Pop top scheduler entry */
  sid = sched_entry->id;
  ret = sched_pop(&ctx->sched);
  if (ret != 0)
    return -1;
  
  /* Get socket */
  map = &ctx->maps[SOCK_MAP];
  sock = ebpf_map_lookup(map->addr, sid, sizeof(struct tcp_sock));
  if (sock == NULL)
    return -1;
  
  /* Do pkt len check to make ebpf verifier happy */
  pktlen_valid = is_pktlen_valid(ctx->pkt, ctx->pkt_end);
  if (!pktlen_valid)
    return -1;
  tcp_pkt = (struct tcp_pkt_inner *) ctx->pkt;
  
  /* Check if payload len valid so ebpf verifier is happy */
  ebpf_spin_lock(&sock->lock);
  hdrlen = tx_get_hdrlen();
  max_paylen = tx_get_max_paylen(ctx, hdrlen);
  tx_bump = tx_get_tx_bump(sock, max_paylen);
  paylen_valid = is_paylen_valid(ctx->pkt, ctx->pkt_end, hdrlen, tx_bump);
  if (!paylen_valid)
  {
    ebpf_spin_unlock(&sock->lock);
    return -1;
  }
  
  /* Fill packet header for transmission */
  tx_fill_hdr(tcp_pkt, sock, hdrlen, tx_bump);
  
  /* Fill payload if this is not a pure ACK */
  if (tx_bump > 0)
  {
    /* Copy payload from TX buffer to packet and update socket */
    payload = ctx->pkt + hdrlen;
    tx_copy_payload(ctx->shm_base, sock, payload, tx_bump);
    tx_sock_tx_bump(sock, tx_bump);

    /* Re-schedule if there are still bytes to send */
    avail  = sock_sched_avail(sock);
    if (avail > 0)
    {
      /* TODO: Get proper priority when using dctcp cc */
      sched_add(&ctx->sched, sock->id, 1, avail);
    }
  }
  
  ebpf_spin_unlock(&sock->lock);
  return hdrlen + tx_bump;
}

SEC("chamelio/event_deq")
int event_deq(struct cham_ebpf_ctx *ctx)
{
  __u8 qe_valid;
  
  qe_valid = is_qe_valid(ctx->qe, ctx->shm_end);
  if (!qe_valid)
    return -1;

  switch (ctx->qe->type)
  {
    case TCP_QUEUE_BUMP_CHAM_TX:
      return deq_handle_bump_tx(ctx);
    case TCP_QUEUE_BUMP_CHAM_RX:
      return deq_handle_bump_rx(ctx);
    case TCP_QUEUE_CTRL_TX:
      return deq_handle_ctl_tx(ctx);
    case TCP_QUEUE_TX_RETRANSMIT:
      return deq_handle_retransmit(ctx);
    default:
      return -1;
  }
  return 0;
}

/***** Miscellaneous helpers *************************************************/

static __always_inline __u8 is_qe_valid(struct queue_entry *qe, void *shm_end)
{
  void *qe_end;
  
  qe_end = (__u8 *) qe + sizeof(struct tcp_queue_bump_entry);
  return qe_end < shm_end;
}

static __always_inline __u8 is_pktlen_valid(void *pkt, void *pkt_end)
{
  return (pkt + sizeof(struct tcp_pkt_inner)) <= pkt_end;
}

static __always_inline __u8 is_paylen_valid(void *pkt, 
    void *pkt_end, __u16 hdrs_len, __u16 paylen)
{
  return (pkt + hdrs_len + paylen) <= pkt_end;
}

static __always_inline void remit(struct cham_scheduler *sched,
    struct tcp_sock *sock)
{
  __u32 avail;
  
  /* Rewind socket */
  sock->tx_avail += sock->tx_pending;
  sock->tx_pending = 0;
  
  /* Remove any scheduled entries */
  sched_remove(sched, sock->id);
  
  /* TODO: Get proper priority when using dctcp cc*/
  /* Add a new retransmit entry */
  avail = sock_sched_avail(sock);
  sched_add(sched, sock->id, 1, avail);
}

/***** RX helpers ************************************************************/

static __always_inline __u32 rx_get_hdrlen(struct tcp_pkt_inner *tcp_pkt)
{
  __u16 ip_hdrs_len, tcp_hdrs_len;
  
  ip_hdrs_len = (__u16) (IPH_HL(&tcp_pkt->ip) * 4);
  tcp_hdrs_len = (__u16) (TCPH_HDRLEN(&tcp_pkt->tcp) * 4);
  return ip_hdrs_len + tcp_hdrs_len;
}

static __always_inline __u32 rx_get_paylen(struct tcp_pkt_inner *tcp_pkt)
{
  __u16 ip_hdrlen, ip_totlen, tcp_hdrlen, paylen;
  
  ip_hdrlen = (__u16) (IPH_HL(&tcp_pkt->ip) * 4);
  ip_totlen = f_beui16(tcp_pkt->ip.len);
  tcp_hdrlen = (__u16) (TCPH_HDRLEN(&tcp_pkt->tcp) * 4);
  paylen = ip_totlen - ip_hdrlen - tcp_hdrlen;
  return paylen;
}

static __always_inline __u8 rx_is_ip(struct tcp_pkt_inner *tcp_pkt)
{
  return IPH_V(&tcp_pkt->ip) == 4 && IPH_HL(&tcp_pkt->ip) >= 5 &&
      tcp_pkt->ip.proto == IP_PROTO_TCP;
}

static __always_inline __u8 rx_is_flags_ctl(struct tcp_pkt_inner *tcp_pkt)
{
  __u16 flags;
  
  flags = TCPH_FLAGS(&tcp_pkt->tcp);
  return flags & (TAS_TCP_SYN | TAS_TCP_FIN | TAS_TCP_RST);
}

static __always_inline __u8 rx_is_ack(struct tcp_pkt_inner *tcp_pkt)
{
  __u16 flags;
  flags = TCPH_FLAGS(&tcp_pkt->tcp);
  return flags & (TAS_TCP_ACK);
}

static __always_inline __u8 rx_is_ack_only(struct tcp_pkt_inner *tcp_pkt)
{
  return rx_get_paylen(tcp_pkt) == 0;
}

static __always_inline __u8 rx_is_payload_dup(struct tcp_pkt_inner *tcp_pkt,
    struct tcp_sock *sock)
{
  __u32 paylen, overlap;
  
  paylen = rx_get_paylen(tcp_pkt);
  overlap = rx_get_payload_overlap(tcp_pkt, sock);
  return paylen <= overlap;
}

static __always_inline __u8 rx_is_out_of_order(struct tcp_pkt_inner *tcp_pkt,
    struct tcp_sock *sock)
{
  __u32 seqno;
  
  seqno = f_beui32(tcp_pkt->tcp.seqno);
  return (__s32) (seqno - sock->rx_seq) > 0;
}

static __always_inline __u8 rx_is_ack_valid(__u32 seqno, 
      __u32 pending, __u32 ackno)
{
  if ((__s32) (ackno - seqno) < 0)
  {
    return 0;
  }
  
  return 1;
}

static __always_inline int rx_punt_ctl(struct cham_ebpf_ctx *ctx,
    struct tcp_pkt_inner *tcp_pkt)
{
  int ret;
  struct equeue *sig_q;
  struct equeue *pkt_q;
  struct tcp_ctrl_cfg *cfg;
  struct tcp_queue_bump_entry *sig_qe;
  struct tcp_queue_bump_entry *pkt_qe;
  struct cham_map *map;

  /* Get control configuration containing queue ids */
  map = &ctx->maps[CFG_MAP];
  cfg = ebpf_map_lookup(map->addr, 0, sizeof(struct tcp_ctrl_cfg));
  if (cfg == NULL)
    return -1;

  /* Get first available entry in end of pkt queue */
  pkt_q = &ctx->equeues[cfg->fast_slow_pkt_qid].eq;
  pkt_qe = ebpf_queue_tail(pkt_q, sizeof(struct tcp_queue_bump_entry));
  if (pkt_qe == NULL)
    return -1;

  /* Get first available entry in end of signal queue */
  sig_q = &ctx->equeues[cfg->fast_slow_sig_qid].eq;
  sig_qe = ebpf_queue_tail(sig_q, sizeof(struct tcp_queue_bump_entry));
  if (sig_qe == NULL)
    return -1;

  /* Copy header to pkt queue */
  ebpf_memcpy(&pkt_qe->data.ctl_pkt.pkt, tcp_pkt, sizeof(struct tcp_pkt_inner));
  
  /* Set ready field of signal entry */
  sig_qe->data.ctl_sig.ready = 1;

  /* Push packet to queue */
  ret = queue_enqueue(pkt_q, TCP_QUEUE_CTRL_RX_PKT);
  if (ret != 0)
    return -1;

  /* Push signal that packet is in queue */
  ret = queue_enqueue(sig_q, TCP_QUEUE_CTRL_RX);
  if (ret != 0)
    return -1;

  return 0;
}

static __always_inline __u32 rx_get_ack_bump(struct tcp_pkt_inner *tcp_pkt,
    struct tcp_sock *sock)
{
  __u8 is_valid;
  __u32 ackno, seqno, pending;
  
  seqno = sock->tx_seq;
  ackno = f_beui32(tcp_pkt->tcp.ackno);
  pending = sock->tx_pending;
  
  /* TODO: Deal with future ACKs */
  is_valid = rx_is_ack_valid(seqno, pending, ackno);
  if (is_valid)
  {
    return ackno - seqno;
  }
  else
  {
    return 0;
  }
}

static __always_inline __u32 rx_get_rx_bump(struct tcp_sock *sock, __u32 paylen)
{
  __u32 free_rx, rx_bump;
  
  free_rx = sock->rx_len - sock->rx_avail;
  rx_bump = paylen;
  if (free_rx < paylen)
  {
    rx_bump = free_rx;
  }
  
  return rx_bump;
}

static __always_inline void rx_sock_bump_ack(struct tcp_sock *sock, 
    __u32 ack_bump)
{
  sock->tx_seq += ack_bump;
  sock->tx_pending -= ack_bump;
  sock->cc_ackb += ack_bump;
  sock->tx_head = (sock->tx_head + ack_bump) % sock->tx_len;
  if (ack_bump > 0)
  {
    sock->cc_acks++;
    sock->rx_dupack_cnt = 0;
    sock->ack_advance_last_tsc = ebpf_rdtsc();
  }
}

static __always_inline void rx_sock_bump_rx(struct tcp_sock *sock, 
    __u32 rx_bump)
{
  sock->rx_seq = sock->rx_seq + rx_bump;
  sock->rx_avail += rx_bump;
}

static __always_inline __u8 rx_should_up_rem_avail(struct tcp_pkt_inner *pkt, 
    struct tcp_sock *sock)
{
  return 0;
}

static __always_inline void rx_sched_ack(struct cham_scheduler *sched,
    struct tcp_sock *sock)
{
  /* TODO: Get proper priority when using dctcp cc */
  sched_add(sched, sock->id, 1, 0);
}

static __always_inline __u32 rx_get_payload_overlap(struct tcp_pkt_inner *tcp_pkt,
    struct tcp_sock *sock)
{
  __u32 seqno, overlap, paylen;
  
  seqno = f_beui32(tcp_pkt->tcp.seqno);
  overlap = 0;
  if ((__s32) (seqno - sock->rx_seq) < 0)
    overlap = sock->rx_seq - seqno;
    
  paylen = rx_get_paylen(tcp_pkt);
  if (overlap > paylen)
    overlap = paylen;

  return overlap;
}

static __always_inline void rx_copy_payload(void *shm_base,
    struct tcp_sock *sock, void *payload, __u32 paylen)
{
  __u8 *rx_base;
  __u32 tail, part;
  
  rx_base = ebpf_map_get(shm_base + sock->rx_off, sock->rx_len);
  tail = sock->rx_head + sock->rx_avail;
  if (tail >= sock->rx_len)
    tail -= sock->rx_len;
  if (tail + paylen <= sock->rx_len)
  {
    ebpf_memcpy(rx_base + tail, payload, paylen);
  }
  else
  {
    part = sock->rx_len - tail;
    ebpf_memcpy(rx_base + tail, payload, part);
    ebpf_memcpy(rx_base, (__u8 *) payload + part, paylen - part);
  }
}

static __always_inline int rx_enqueue_rx_bump(struct equeue *q,
    struct tcp_sock *sock, __u32 rx_bump,
    __u16 rx_port, __u32 rx_ip)
{
  int ret;
  struct tcp_queue_bump_entry *qe;
  struct tcp_queue_bump_app_rx *bump;
  
  qe = ebpf_queue_tail(q, sizeof(struct tcp_queue_bump_entry));
  if (qe == NULL)
  {
    return -1;
  }
  
  bump = &qe->data.bump_app_rx;
  bump->opaque = sock->opaque;
  bump->rx_avail = rx_bump;
  bump->rx_port = rx_port;
  bump->rx_ip = rx_ip;

  ret = queue_enqueue(q, TCP_QUEUE_BUMP_APP_RX);
  if (ret != 0)
  {
    return -1;
  }

  return 0;
}

static __always_inline int rx_enqueue_tx_bump(struct equeue *q,
    struct tcp_sock *sock, __u32 tx_bump)
{
  int ret;
  struct tcp_queue_bump_entry *qe;
  struct tcp_queue_bump_app_tx *bump;
  
  qe = ebpf_queue_tail(q, sizeof(struct tcp_queue_bump_entry));
  if (qe == NULL)
  {
    return -1;
  }
  
  bump = &qe->data.bump_app_tx;
  bump->opaque = sock->opaque;
  bump->tx_head = tx_bump;

  ret = queue_enqueue(q, TCP_QUEUE_BUMP_APP_TX);
  if (ret != 0)
  {
    return -1;
  }

  return 0;
}

/***** TX helpers ************************************************************/

static __always_inline __u32 tx_get_hdrlen()
{
  return sizeof(struct ip_hdr) + TCP_HLEN;
}

static __always_inline __u32 tx_get_max_paylen(struct cham_ebpf_ctx *ctx,
    __u32 hdrlen)
{
  __u32 payload_headroom;
  
  payload_headroom = (ctx->pkt_end - ctx->pkt);
  if (payload_headroom < hdrlen)
  {
    return 0;
  }
  
  return payload_headroom - hdrlen;
}

static __always_inline __u32 tx_get_tx_bump(struct tcp_sock *sock,
    __u32 max_paylen)
{
  __u32 tx_bump;
  __s64 fc_avail;
  
  tx_bump = sock->tx_avail;
  if (tx_bump > max_paylen)
    tx_bump = max_paylen;
    
  fc_avail = (__s64) sock->tx_remote_avail - (__s64) sock->tx_pending;
  if (fc_avail < 0)
    fc_avail = 0;
    
  if (tx_bump > fc_avail)
    tx_bump = fc_avail;
  
  return tx_bump;
}

static __always_inline void tx_fill_hdr(struct tcp_pkt_inner *tcp_pkt,
    struct tcp_sock *sock, __u32 hdrlen, __u32 paylen)
{
  __u32 rxwnd;
  /* Fill IP header */
  IPH_VHL_SET(&tcp_pkt->ip, 4, 5);
  tcp_pkt->ip._tos = 0;
  tcp_pkt->ip.len = t_beui16(hdrlen + paylen);
  tcp_pkt->ip.id = t_beui16(3);
  tcp_pkt->ip.offset = t_beui16(0);
  tcp_pkt->ip.ttl = 0xff;
  tcp_pkt->ip.proto = IP_PROTO_TCP;
  tcp_pkt->ip.chksum = 0;
  tcp_pkt->ip.src = t_beui32(sock->local_ip);
  tcp_pkt->ip.dst = t_beui32(sock->remote_ip);
  if (sock->flags & TCP_SOCK_FLAG_ECN)
  {
    IPH_ECN_SET(&tcp_pkt->ip, CHAM_IP_ECN_ECT0);
  }
  
  /* Fill TCP header */
  rxwnd = sock->rx_len - sock->rx_avail;
  tcp_pkt->tcp.src = t_beui16(sock->local_port);
  tcp_pkt->tcp.dest = t_beui16(sock->remote_port);
  tcp_pkt->tcp.seqno = t_beui32(sock->tx_seq + sock->tx_pending);
  tcp_pkt->tcp.ackno = t_beui32(sock->rx_seq);
  TCPH_HDRLEN_FLAGS_SET(&tcp_pkt->tcp, 5, TAS_TCP_PSH | TAS_TCP_ACK);
  tcp_pkt->tcp.wnd = t_beui16(rxwnd);
  tcp_pkt->tcp.chksum = 0;
  tcp_pkt->tcp.urgp = t_beui16(0);
}

static __always_inline void tx_copy_payload(void *shm_base, struct tcp_sock *sock,
    void *payload, __u32 tx_bump)
{
  __u32 tx_pos, part;
  
  tx_pos = sock->tx_head + sock->tx_pending;
  if (tx_pos >= sock->tx_len)
  {
    tx_pos -= sock->tx_len;
  }
  
  if (tx_pos + tx_bump <= sock->tx_len)
  {
    ebpf_memcpy(payload, shm_base + sock->tx_off + tx_pos, tx_bump);
  }
  else
  {
    part = sock->tx_len - tx_pos;
    ebpf_memcpy(payload, shm_base + sock->tx_off + tx_pos, part);
    ebpf_memcpy(payload + part, shm_base + sock->tx_off, tx_bump - part);
  }
}

static __always_inline void tx_sock_tx_bump(struct tcp_sock *sock,
    __u32 tx_bump)
{
  sock->tx_avail -= tx_bump;
  sock->tx_pending += tx_bump;
}

/***** DEQ helpers ***********************************************************/

static __always_inline int deq_handle_bump_tx(struct cham_ebpf_ctx *ctx)
{
  struct tcp_sock *sock;
  struct tcp_queue_bump_entry *qe;
  struct tcp_queue_bump_cham_tx *bump;
  struct cham_map *map;

  qe = (struct tcp_queue_bump_entry *) ctx->qe;
  bump = &qe->data.bump_cham_tx;

  /* Return if we can't find socket */
  map = &ctx->maps[SOCK_MAP];
  sock = ebpf_map_lookup(map->addr, bump->sock_id, sizeof(struct tcp_sock));
  if (sock == NULL)
    return -1;

  /* Update socket state */
  ebpf_spin_lock(&sock->lock);
  deq_sock_bump_tx(sock, bump);  
  ebpf_spin_unlock(&sock->lock);
  
  /* Schedule bytes for transmission */
  sched_add(&ctx->sched, sock->id, 1, bump->tx_avail);
  
  return 0;
}

static __always_inline int deq_handle_bump_rx(struct cham_ebpf_ctx *ctx)
{
  __u8 pktlen_valid, rxfull;
  __u32 hdrlen;
  struct tcp_sock *sock;
  struct tcp_queue_bump_cham_rx *bump;
  struct tcp_queue_bump_entry *qe;
  struct cham_map *map;

  qe = (struct tcp_queue_bump_entry *) ctx->qe;
  bump = &qe->data.bump_cham_rx;

  /* Return if we can't find socket */
  map = &ctx->maps[SOCK_MAP];
  sock = ebpf_map_lookup(map->addr, bump->sock_id, sizeof(struct tcp_sock));
  if (sock == NULL)
    return -1;
  
  ebpf_spin_lock(&sock->lock);
  rxfull = sock->rx_len == sock->rx_avail;
  
  /* Update socket state */
  deq_sock_bump_rx(sock, bump);
  
  /* If congestion window opened schedule a ACK */
  if (rxfull)
  {
    pktlen_valid = is_pktlen_valid(ctx->pkt, ctx->pkt_end);
    if (!pktlen_valid)
    {
      ebpf_spin_unlock(&sock->lock);
      return -1;
    }
    
    hdrlen = tx_get_hdrlen();
    tx_fill_hdr(ctx->pkt, sock, hdrlen, 0);
    ebpf_spin_unlock(&sock->lock);
    return hdrlen;
  }
  
  ebpf_spin_unlock(&sock->lock);

  return 0;
}

static __always_inline int deq_handle_ctl_tx(struct cham_ebpf_ctx *ctx)
{
  int ret;
  __u32 hdrlen;
  struct cham_map *map;
  struct tcp_ctrl_cfg *cfg;
  struct dqueue *ctl_pkt_q;
  struct tcp_queue_bump_entry *ctl_pkt_qe;
  struct tcp_pkt_inner *ctl_pkt;

  /* Return if we can't find map containing queue ids */
  map = &ctx->maps[CFG_MAP];
  cfg = ebpf_map_lookup(map->addr, 0, sizeof(struct tcp_ctrl_cfg));
  if (cfg == NULL)
    return -1;
  
  /* Get control packet */
  ctl_pkt_q = &ctx->dqueues[cfg->slow_fast_pkt_qid].dq;
  ctl_pkt_qe = ebpf_queue_head(ctl_pkt_q, sizeof(struct tcp_queue_bump_entry));
  if (ctl_pkt_qe == NULL)
    return -1;
  ctl_pkt = &ctl_pkt_qe->data.ctl_pkt.pkt;
  
  /* Copy control packet for transmission */
  hdrlen = f_beui16(ctl_pkt->ip.len);
  ctl_pkt->ip.chksum = 0;
  ctl_pkt->tcp.chksum = 0;
  ebpf_memcpy(ctx->pkt, ctl_pkt, hdrlen);
  
  ret = queue_dequeue(ctl_pkt_q);
  if (ret != 0)
    return -1;

  return hdrlen;
}

static __always_inline int deq_handle_retransmit(struct cham_ebpf_ctx *ctx)
{
  struct tcp_sock *sock;
  struct tcp_queue_bump_entry *qe;
  struct tcp_queue_ctl_remit *cmd;
  struct cham_map *map;
  
  qe = (struct tcp_queue_bump_entry *) ctx->qe;
  cmd = &qe->data.fast_sock;

  /* Return if we can't find socket */
  map = &ctx->maps[SOCK_MAP];
  sock = ebpf_map_lookup(map->addr, cmd->sock_id, sizeof(struct tcp_sock));
  if (sock == NULL)
    return -1;
  
  /* Rewind socket and schedule retransmission */
  ebpf_spin_lock(&sock->lock);
  remit(&ctx->sched, sock);
  ebpf_spin_unlock(&sock->lock);
  
  return 0;
}

static __always_inline __u32 deq_get_tx_ip(struct tcp_sock *sock,
    struct tcp_queue_bump_cham_tx *bump)
{
  if (bump->tx_ip == 0)
  {
    return sock->remote_ip;
  }
  else
  {
    return bump->tx_ip;
  }
}

static __always_inline __u32 deq_get_tx_port(struct tcp_sock *sock,
    struct tcp_queue_bump_cham_tx *bump)
{
  if (bump->tx_port == 0)
  {
    return sock->remote_port;
  }
  else
  {
    return bump->tx_port;
  }
}

static __always_inline __u32 deq_sock_bump_tx(struct tcp_sock *sock,
    struct tcp_queue_bump_cham_tx *bump)
{
  __u32 tx_ip;
  __u32 tx_port;
  
  tx_ip = deq_get_tx_ip(sock, bump);
  tx_port = deq_get_tx_port(sock, bump);
  sock->tx_avail += bump->tx_avail;
  sock->remote_ip = tx_ip;
  sock->remote_port = tx_port;
}

static __always_inline __u32 deq_sock_bump_rx(struct tcp_sock *sock,
    struct tcp_queue_bump_cham_rx *bump)
{
  __u32 new_head;
  
  new_head = sock->rx_head + bump->rx_head;
  if (new_head >= sock->rx_len)
    new_head -= sock->rx_len;
  
  sock->rx_head = new_head;
  sock->rx_avail -= bump->rx_head;
}

/***** Socket helpers ********************************************************/

static __always_inline struct tcp_sock * sock_find(struct cham_ebpf_ctx *ctx,
    struct tcp_pkt_inner *tcp_pkt)
{
  int i;
  __u16 lport, rport;
  __u32 sid;
  __u32 hash;
  __u32 lip, rip;
  struct tcp_flow_bucket *bucket;
  struct tcp_sock *sock;
  struct cham_map *map;

  rip = f_beui32(tcp_pkt->ip.src);
  lip = f_beui32(tcp_pkt->ip.dst);
  rport = f_beui16(tcp_pkt->tcp.src);
  lport = f_beui16(tcp_pkt->tcp.dest);
  
  hash = sock_hash(lip, lport, rip, rport) % TCP_FLOW_BUCKETS;

  /* Get map hash buckets with potential socket ids */
  map = &ctx->maps[SID_BUCK_MAP];
  bucket = ebpf_map_lookup(map->addr, hash, sizeof(struct tcp_flow_bucket));
  if (bucket == NULL)
    return NULL;

  /* Iterate over hash buckets to get actual id */
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

    if (sock->local_ip == lip && sock->local_port == lport &&
        sock->remote_ip == rip && sock->remote_port == rport)
      return sock;
  }

  return NULL;
}

static __always_inline __u32 sock_hash(__u32 lip, __u16 lport,
    __u32 rip, __u16 rport)
{
  __u32 h;

  h = lip ^ rip ^ ((__u32) lport << 16) ^ rport;
  return h ^ (h >> 16);
}

static __always_inline __u32 sock_sched_avail(struct tcp_sock *sock)
{
  __u32 fc_avail;
  __u32 avail;
  
  if (sock->tx_pending >= sock->tx_remote_avail)
    return 0;
    
  fc_avail = sock->tx_remote_avail - sock->tx_pending;
  avail = fc_avail;
  if (sock->tx_avail < fc_avail)
    avail = sock->tx_avail;
  return avail;
}

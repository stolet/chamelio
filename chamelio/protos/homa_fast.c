#include <linux/types.h>
#include "native_fast.h"

#include "cham_fast.h"
#include "homa.h"
#include "homa_hdr.h"
#include "homa_queue_types.h"

#define IPV4_FRAGMENT_MASK 0x3fff
#define HOMA_IP_TTL 64

/* Packet and queue bounds. */
static __always_inline __u8 is_paylen_valid(struct homa_pkt *p,
    void *pkt_end, __u32 len);
static __always_inline __u8 deq_is_qe_valid(struct homa_queue_entry *qe,
    void *shm_end);

/* RX helpers. */
static __always_inline __u8 rx_is_pkt_valid(struct homa_pkt *p, void *pkt_end);
static __always_inline __u64 rx_get_local_rpc_id(struct homa_pkt *p);
static __always_inline struct homa_rpc *rx_rpc_lookup(
    struct cham_ebpf_ctx *ctx, __u64 id, __u32 sock_id, __u32 remote_ip,
    __u16 remote_port, __u32 *slot);

/* Socket and RPC helpers. */
static __always_inline struct homa_socket *sock_find(struct cham_ebpf_ctx *ctx,
    struct homa_pkt *p, __u32 *sock_id);
static __always_inline __u8 sock_is_rx_match(struct homa_socket *sock,
    struct homa_pkt *p);
static __always_inline __u8 rpc_is_server(__u64 id);
static __always_inline __u32 rpc_get_client_slot(__u64 id);
static __always_inline __u8 rpc_is_response_match(struct homa_rpc *rpc,
    __u64 id, __u32 sock_id, __u32 remote_ip, __u16 remote_port);
static __always_inline __u8 rpc_is_tx_ready(struct homa_rpc *rpc);

/* TX helpers. */
static __always_inline void tx_fill_ip_hdr(struct homa_pkt *p,
    struct homa_socket *sock, struct homa_rpc *rpc, __u32 len);
static __always_inline void tx_fill_data_hdr(struct homa_data_hdr *data,
    struct homa_socket *sock, struct homa_rpc *rpc, __u32 len);

int homa_fast_event_rx(struct cham_ebpf_ctx *ctx)
{
  struct homa_pkt *p = ctx->pkt;
  struct homa_socket *sock;
  struct homa_rpc *rpc;
  struct homa_queue_entry *qe;
  struct equeue *q;
  __u32 sock_id, slot, len;
  __u64 id;

  if (!rx_is_pkt_valid(p, ctx->pkt_end))
    return -1;

  len = f_beui32(p->data.message_length);
  sock = sock_find(ctx, p, &sock_id);
  if (sock == NULL)
    return -1;

  id = rx_get_local_rpc_id(p);
  rpc = rx_rpc_lookup(ctx, id, sock_id, f_beui32(p->ip.src),
      f_beui16(p->data.common.sport), &slot);
  if (rpc == NULL)
    return -1;

  if (rpc_is_server(id))
  {
    rpc->id = id;
    rpc->app_qid = sock->app_qid;
    rpc->sock_id = sock_id;
    rpc->remote_ip = f_beui32(p->ip.src);
    rpc->remote_port = f_beui16(p->data.common.sport);
  }

  if (rpc->app_qid >= MAX_PROTO_QUEUES)
    return -1;

  q = &ctx->equeues[rpc->app_qid].eq;
  qe = ebpf_queue_tail(q, sizeof(*qe));
  if (qe == NULL)
    return -1;

  ebpf_memcpy(rpc->rx_data, (__u8 *) p + sizeof(*p), len);
  rpc->rx_len = len;
  rpc->state = rpc_is_server(id) ?
      HOMA_RPC_REQUEST_READY : HOMA_RPC_RESPONSE_READY;
  qe->data.rpc_slot = slot;
  return queue_enqueue(q, HOMA_QUEUE_RX);
}

int homa_fast_event_sched(struct cham_ebpf_ctx *ctx)
{
  return 0;
}

int homa_fast_event_deq(struct cham_ebpf_ctx *ctx)
{
  struct homa_pkt *p = ctx->pkt;
  struct homa_queue_entry *qe = (struct homa_queue_entry *) ctx->qe;
  struct homa_rpc *rpc;
  struct homa_socket *sock;
  __u32 slot, len;

  if (!deq_is_qe_valid(qe, ctx->shm_end))
    return -1;

  slot = qe->data.rpc_slot;
  if (slot >= HOMA_MAX_RPCS)
    return -1;

  rpc = ebpf_map_lookup(ctx->maps[HOMA_RPC_MAP].addr, slot, sizeof(*rpc));
  if (!rpc_is_tx_ready(rpc))
    return -1;

  sock = ebpf_map_lookup(ctx->maps[HOMA_SOCKET_MAP].addr, rpc->sock_id,
      sizeof(*sock));
  if (sock == NULL || sock->state != HOMA_SOCKET_BOUND)
    return 0;

  len = rpc->tx_len;
  if (!is_paylen_valid(p, ctx->pkt_end, len))
    return -1;

  tx_fill_ip_hdr(p, sock, rpc, len);
  tx_fill_data_hdr(&p->data, sock, rpc, len);
  ebpf_memcpy((__u8 *) p + sizeof(*p), rpc->tx_data, len);

  if (rpc_is_server(rpc->id))
    rpc->state = HOMA_RPC_RESPONSE_SENT;

  return sizeof(*p) + len;
}

static __always_inline __u8 is_paylen_valid(struct homa_pkt *p,
    void *pkt_end, __u32 len)
{
  __u8 single_packet = len <= HOMA_MAX_PAYLOAD;
  __u8 in_bounds = (__u8 *) p + sizeof(*p) + len <= (__u8 *) pkt_end;

  return single_packet && in_bounds;
}

static __always_inline __u8 deq_is_qe_valid(struct homa_queue_entry *qe,
    void *shm_end)
{
  if ((__u8 *) qe + sizeof(*qe) > (__u8 *) shm_end)
    return 0;

  return qe->type == HOMA_QUEUE_TX;
}

static __always_inline __u8 rx_is_pkt_valid(struct homa_pkt *p, void *pkt_end)
{
  __u32 len;
  __u8 ipv4, fixed_ip_hdr, homa, unfragmented;
  __u8 data_pkt, data_hdr, whole_message, unscheduled, ip_len_valid;

  if ((__u8 *) p + sizeof(*p) > (__u8 *) pkt_end)
    return 0;

  ipv4 = IPH_V(&p->ip) == 4;
  fixed_ip_hdr = IPH_HL(&p->ip) == sizeof(p->ip) / sizeof(__u32);
  homa = p->ip.proto == IP_PROTO_HOMA;
  unfragmented = !(f_beui16(p->ip.offset) & IPV4_FRAGMENT_MASK);
  if (!ipv4 || !fixed_ip_hdr || !homa || !unfragmented)
    return 0;

  len = f_beui32(p->data.message_length);
  data_pkt = p->data.common.type == HOMA_DATA;
  data_hdr = p->data.common.doff == ((sizeof(p->data) / sizeof(__u32)) << 4);
  whole_message = f_beui32(p->data.offset) == 0 &&
      f_beui32(p->data.segment_length) == len;
  unscheduled = f_beui32(p->data.incoming) >= len;
  ip_len_valid = f_beui16(p->ip.len) == sizeof(*p) + len;

  return data_pkt && data_hdr && whole_message && unscheduled &&
      ip_len_valid && is_paylen_valid(p, pkt_end, len);
}

static __always_inline __u64 rx_get_local_rpc_id(struct homa_pkt *p)
{
  /* The receiver uses the opposite role bit for the same RPC. */
  return f_beui64(p->data.common.sender_id) ^ HOMA_RPC_SERVER_BIT;
}

static __always_inline struct homa_rpc *rx_rpc_lookup(
    struct cham_ebpf_ctx *ctx, __u64 id, __u32 sock_id, __u32 remote_ip,
    __u16 remote_port, __u32 *slot)
{
  struct homa_rpc *rpc;
  struct homa_state *state;
  __u32 i;
  __u64 max_client_id = HOMA_CLIENT_RPCS * HOMA_RPC_ID_STRIDE;

  if (!rpc_is_server(id))
  {
    if (id < HOMA_RPC_ID_STRIDE || id > max_client_id)
      return NULL;

    i = rpc_get_client_slot(id);
    rpc = ebpf_map_lookup(ctx->maps[HOMA_RPC_MAP].addr, i, sizeof(*rpc));
    if (!rpc_is_response_match(rpc, id, sock_id, remote_ip, remote_port))
      return NULL;

    *slot = i;
    return rpc;
  }

  state = ebpf_map_lookup(ctx->maps[HOMA_STATE_MAP].addr, 0, sizeof(*state));
  if (state == NULL || state->incoming_rpcs >= HOMA_CLIENT_RPCS)
    return NULL;

  i = HOMA_CLIENT_RPCS + state->incoming_rpcs;
  rpc = ebpf_map_lookup(ctx->maps[HOMA_RPC_MAP].addr, i, sizeof(*rpc));
  if (rpc == NULL)
    return NULL;

  state->incoming_rpcs++;
  *slot = i;
  return rpc;
}

static __always_inline struct homa_socket *sock_find(struct cham_ebpf_ctx *ctx,
    struct homa_pkt *p, __u32 *sock_id)
{
  struct homa_socket *sock;
  __u32 *port;

  port = ebpf_map_lookup(ctx->maps[HOMA_PORT_MAP].addr,
      f_beui16(p->data.common.dport), sizeof(*port));
  if (port == NULL)
    return NULL;

  if (*port == 0 || *port > HOMA_MAX_SOCKETS)
    return NULL;

  *sock_id = *port - 1;
  sock = ebpf_map_lookup(ctx->maps[HOMA_SOCKET_MAP].addr, *sock_id,
      sizeof(*sock));
  if (!sock_is_rx_match(sock, p))
    return NULL;

  return sock;
}

static __always_inline __u8 sock_is_rx_match(struct homa_socket *sock,
    struct homa_pkt *p)
{
  __u8 bound, ip_match, port_match;

  if (sock == NULL)
    return 0;

  bound = sock->state == HOMA_SOCKET_BOUND;
  ip_match = sock->local_ip == f_beui32(p->ip.dst);
  port_match = sock->local_port == f_beui16(p->data.common.dport);
  return bound && ip_match && port_match;
}

static __always_inline __u8 rpc_is_server(__u64 id)
{
  return (id & HOMA_RPC_SERVER_BIT) != 0;
}

static __always_inline __u32 rpc_get_client_slot(__u64 id)
{
  /* Invert the library's client ID encoding: (slot + 1) * stride. */
  __u64 client_handle = id / HOMA_RPC_ID_STRIDE;

  return client_handle - 1;
}

static __always_inline __u8 rpc_is_response_match(struct homa_rpc *rpc,
    __u64 id, __u32 sock_id, __u32 remote_ip, __u16 remote_port)
{
  __u8 waiting, same_rpc, same_socket, same_peer;

  if (rpc == NULL)
    return 0;

  waiting = rpc->state == HOMA_RPC_WAIT_RESPONSE;
  same_rpc = rpc->id == id;
  same_socket = rpc->sock_id == sock_id;
  same_peer = rpc->remote_ip == remote_ip && rpc->remote_port == remote_port;
  return waiting && same_rpc && same_socket && same_peer;
}

static __always_inline __u8 rpc_is_tx_ready(struct homa_rpc *rpc)
{
  __u8 valid_socket, waiting_response, request_ready;

  if (rpc == NULL)
    return 0;

  valid_socket = rpc->sock_id < HOMA_MAX_SOCKETS;
  waiting_response = rpc->state == HOMA_RPC_WAIT_RESPONSE;
  request_ready = rpc->state == HOMA_RPC_REQUEST_READY;
  return valid_socket && (waiting_response || request_ready);
}

static __always_inline void tx_fill_ip_hdr(struct homa_pkt *p,
    struct homa_socket *sock, struct homa_rpc *rpc, __u32 len)
{
  IPH_VHL_SET(&p->ip, 4, sizeof(p->ip) / sizeof(__u32));
  p->ip._tos = 0;
  p->ip.len = t_beui16(sizeof(*p) + len);
  p->ip.id = t_beui16(0);
  p->ip.offset = t_beui16(0);
  p->ip.ttl = HOMA_IP_TTL;
  p->ip.proto = IP_PROTO_HOMA;
  p->ip.chksum = 0;
  p->ip.src = t_beui32(sock->local_ip);
  p->ip.dst = t_beui32(rpc->remote_ip);
}

static __always_inline void tx_fill_data_hdr(struct homa_data_hdr *data,
    struct homa_socket *sock, struct homa_rpc *rpc, __u32 len)
{
  data->common.sport = t_beui16(sock->local_port);
  data->common.dport = t_beui16(rpc->remote_port);
  data->common.unused[0] = 0;
  data->common.unused[1] = 0;
  data->common.doff = (sizeof(*data) / sizeof(__u32)) << 4;
  data->common.type = HOMA_DATA;
  data->common.seq = 0;
  data->common.checksum = 0;
  data->common.unused4 = 0;
  data->common.sender_id = t_beui64(rpc->id);
  data->message_length = t_beui32(len);
  data->incoming = t_beui32(len);
  data->cutoff_version = t_beui16(0);
  data->retransmit = 0;
  data->unused = 0;
  data->offset = t_beui32(0);
  data->segment_length = t_beui32(len);
  data->ack_id = t_beui64(0);
  data->ack_sport = t_beui16(0);
  data->ack_dport = t_beui16(0);
}

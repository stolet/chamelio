#include <linux/types.h>
#include "native_fast.h"

#include "cham_fast.h"
#include "homa.h"
#include "homa_hdr.h"
#include "homa_queue_types.h"

static __always_inline struct homa_rpc *receive_rpc(struct cham_ebpf_ctx *ctx,
    __u64 id, __u32 sock_id, __u32 remote_ip, __u16 remote_port,
    __u32 *slot);

int homa_fast_event_rx(struct cham_ebpf_ctx *ctx)
{
  struct homa_pkt *p = ctx->pkt;
  struct homa_socket *sock;
  struct homa_rpc *rpc;
  struct homa_queue_entry *qe;
  struct equeue *q;
  __u32 *port;
  __u32 sock_id, slot, len;
  __u64 id;

  if ((__u8 *) p + sizeof(*p) > (__u8 *) ctx->pkt_end)
    return -1;

  if (IPH_V(&p->ip) != 4 || IPH_HL(&p->ip) != 5 ||
      p->ip.proto != IP_PROTO_HOMA || (f_beui16(p->ip.offset) & 0x3fff))
    return -1;

  len = f_beui32(p->data.message_length);
  if (p->data.common.type != HOMA_DATA || len > HOMA_MAX_PAYLOAD ||
      p->data.common.doff != ((sizeof(p->data) / 4) << 4) ||
      f_beui32(p->data.offset) != 0 ||
      f_beui32(p->data.segment_length) != len ||
      f_beui32(p->data.incoming) < len ||
      f_beui16(p->ip.len) != sizeof(*p) + len ||
      (__u8 *) p + sizeof(*p) + len > (__u8 *) ctx->pkt_end)
    return -1;

  port = ebpf_map_lookup(ctx->maps[HOMA_PORT_MAP].addr,
      f_beui16(p->data.common.dport), sizeof(*port));
  if (port == NULL || *port == 0 || *port > HOMA_MAX_SOCKETS)
    return -1;

  sock_id = *port - 1;
  sock = ebpf_map_lookup(ctx->maps[HOMA_SOCKET_MAP].addr, sock_id,
      sizeof(*sock));
  if (sock == NULL || sock->state != HOMA_SOCKET_BOUND ||
      sock->local_ip != f_beui32(p->ip.dst) ||
      sock->local_port != f_beui16(p->data.common.dport))
    return -1;

  id = f_beui64(p->data.common.sender_id) ^ 1;
  rpc = receive_rpc(ctx, id, sock_id, f_beui32(p->ip.src),
      f_beui16(p->data.common.sport), &slot);
  if (rpc == NULL)
    return -1;

  if (id & 1)
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
  rpc->state = (id & 1) ? HOMA_RPC_REQUEST_READY : HOMA_RPC_RESPONSE_READY;
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

  if ((__u8 *) qe + sizeof(*qe) > (__u8 *) ctx->shm_end ||
      qe->type != HOMA_QUEUE_TX)
    return -1;

  slot = qe->data.rpc_slot;
  if (slot >= HOMA_MAX_RPCS)
    return -1;

  rpc = ebpf_map_lookup(ctx->maps[HOMA_RPC_MAP].addr, slot, sizeof(*rpc));
  if (rpc == NULL || rpc->sock_id >= HOMA_MAX_SOCKETS ||
      (rpc->state != HOMA_RPC_WAIT_RESPONSE &&
      rpc->state != HOMA_RPC_REQUEST_READY))
    return -1;

  sock = ebpf_map_lookup(ctx->maps[HOMA_SOCKET_MAP].addr, rpc->sock_id,
      sizeof(*sock));
  if (sock == NULL || sock->state != HOMA_SOCKET_BOUND)
    return 0;

  len = rpc->tx_len;
  if (len > HOMA_MAX_PAYLOAD ||
      (__u8 *) p + sizeof(*p) + len > (__u8 *) ctx->pkt_end)
    return -1;

  IPH_VHL_SET(&p->ip, 4, 5);
  p->ip._tos = 0;
  p->ip.len = t_beui16(sizeof(*p) + len);
  p->ip.id = t_beui16(0);
  p->ip.offset = t_beui16(0);
  p->ip.ttl = 64;
  p->ip.proto = IP_PROTO_HOMA;
  p->ip.chksum = 0;
  p->ip.src = t_beui32(sock->local_ip);
  p->ip.dst = t_beui32(rpc->remote_ip);

  p->data.common.sport = t_beui16(sock->local_port);
  p->data.common.dport = t_beui16(rpc->remote_port);
  p->data.common.unused[0] = 0;
  p->data.common.unused[1] = 0;
  p->data.common.doff = (sizeof(p->data) / 4) << 4;
  p->data.common.type = HOMA_DATA;
  p->data.common.seq = 0;
  p->data.common.checksum = 0;
  p->data.common.unused4 = 0;
  p->data.common.sender_id = t_beui64(rpc->id);
  p->data.message_length = t_beui32(len);
  p->data.incoming = t_beui32(len);
  p->data.cutoff_version = t_beui16(0);
  p->data.retransmit = 0;
  p->data.unused = 0;
  p->data.offset = t_beui32(0);
  p->data.segment_length = t_beui32(len);
  p->data.ack_id = t_beui64(0);
  p->data.ack_sport = t_beui16(0);
  p->data.ack_dport = t_beui16(0);
  ebpf_memcpy((__u8 *) p + sizeof(*p), rpc->tx_data, len);

  if (rpc->id & 1)
    rpc->state = HOMA_RPC_RESPONSE_SENT;

  return sizeof(*p) + len;
}

static __always_inline struct homa_rpc *receive_rpc(struct cham_ebpf_ctx *ctx,
    __u64 id, __u32 sock_id, __u32 remote_ip, __u16 remote_port,
    __u32 *slot)
{
  struct homa_rpc *rpc;
  struct homa_state *state;
  __u32 i;

  if (!(id & 1))
  {
    if (id < 2 || id / 2 > HOMA_CLIENT_RPCS)
      return NULL;

    i = id / 2 - 1;
    rpc = ebpf_map_lookup(ctx->maps[HOMA_RPC_MAP].addr, i, sizeof(*rpc));
    if (rpc == NULL || rpc->state != HOMA_RPC_WAIT_RESPONSE ||
        rpc->id != id || rpc->sock_id != sock_id ||
        rpc->remote_ip != remote_ip || rpc->remote_port != remote_port)
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

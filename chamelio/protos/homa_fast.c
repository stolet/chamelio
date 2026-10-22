#include <linux/types.h>
#include "native_fast.h"

#include "cham_fast.h"
#include "homa.h"
#include "homa_hdr.h"
#include "homa_queue_types.h"

#define IPV4_FRAGMENT_MASK 0x3fff
#define HOMA_IP_TTL 64

static __always_inline struct homa_rpc *rpc_at(struct cham_ebpf_ctx *ctx,
    __u32 slot);
static __always_inline struct homa_socket *sock_at(struct cham_ebpf_ctx *ctx,
    __u32 sock_id);
static __always_inline struct homa_state *state_get(struct cham_ebpf_ctx *ctx);
static __always_inline __u32 rpc_hash(__u64 id, __u32 sock_id,
    __u32 remote_ip, __u16 remote_port);
static __always_inline struct homa_rpc *rpc_server_find(
    struct cham_ebpf_ctx *ctx, __u64 id, __u32 sock_id, __u32 remote_ip,
    __u16 remote_port, __u32 *slot);
static __always_inline void rpc_server_insert(struct cham_ebpf_ctx *ctx,
    struct homa_rpc *rpc, __u32 slot);
static __always_inline void rpc_server_remove(struct cham_ebpf_ctx *ctx,
    struct homa_rpc *rpc);
static __always_inline void rpc_free(struct cham_ebpf_ctx *ctx,
    struct homa_rpc *rpc, __u32 slot);
static __always_inline void rpc_reclaim(struct cham_ebpf_ctx *ctx,
    struct homa_rpc *rpc, __u32 slot);
static __always_inline void rpc_schedule(struct cham_ebpf_ctx *ctx,
    __u32 slot, __u32 delay_us);
static __always_inline void rx_ack_server(struct cham_ebpf_ctx *ctx,
    __u64 client_id, __u32 remote_ip, __u16 client_port, __u16 server_port);
static __always_inline struct homa_rpc *rx_rpc_lookup(
    struct cham_ebpf_ctx *ctx, __u64 id, __u32 sock_id, __u32 remote_ip,
    __u16 remote_port, __u32 *slot);
static __always_inline int rx_handle_data(struct cham_ebpf_ctx *ctx);
static __always_inline int rx_handle_ack(struct cham_ebpf_ctx *ctx);
static __always_inline int rx_handle_need_ack(struct cham_ebpf_ctx *ctx);
static __always_inline int deq_handle_tx(struct cham_ebpf_ctx *ctx,
    struct homa_rpc *rpc, __u32 slot);
static __always_inline int tx_segment(struct cham_ebpf_ctx *ctx,
    struct homa_rpc *rpc, __u32 slot);
static __always_inline int tx_control(struct cham_ebpf_ctx *ctx,
    struct homa_socket *sock, __u32 remote_ip, __u16 remote_port,
    __u64 id, __u8 type);
static __always_inline __u8 rx_is_common_valid(struct homa_pkt *p,
    void *pkt_end);
static __always_inline void tx_fill_ip_hdr(struct ip_hdr *ip,
    __u32 local_ip, __u32 remote_ip, __u32 len);
static __always_inline void tx_fill_common_hdr(struct homa_common_hdr *common,
    __u16 local_port, __u16 remote_port, __u64 id, __u8 type);
static __always_inline __u8 is_paylen_valid(struct homa_pkt *p,
    void *pkt_end, __u32 len);
static __always_inline __u8 rx_is_pkt_valid(struct homa_pkt *p, void *pkt_end);
static __always_inline __u64 rx_get_local_rpc_id(struct homa_pkt *p);
static __always_inline struct homa_socket *sock_find(struct cham_ebpf_ctx *ctx,
    struct homa_pkt *p, __u32 *sock_id);
static __always_inline __u8 sock_is_rx_match(struct homa_socket *sock,
    struct homa_pkt *p);
static __always_inline __u8 rpc_is_server(__u64 id);
static __always_inline __u32 rpc_get_client_slot(__u64 id);
static __always_inline __u8 rpc_is_response_match(struct homa_rpc *rpc,
    __u64 id, __u32 sock_id, __u32 remote_ip, __u16 remote_port);
static __always_inline __u8 rpc_is_tx_ready(struct homa_rpc *rpc);
static __always_inline void tx_fill_data_hdr(struct homa_data_hdr *data,
    struct homa_socket *sock, struct homa_rpc *rpc, __u32 len);

static __always_inline __u32 tx_initial_credit(struct cham_ebpf_ctx *ctx,
    __u32 len);
static __always_inline void grant_wake(struct cham_ebpf_ctx *ctx);
static __always_inline void grant_append(struct cham_ebpf_ctx *ctx,
    struct homa_rpc *rpc, __u32 slot);
static __always_inline void grant_remove(struct cham_ebpf_ctx *ctx,
    struct homa_rpc *rpc);
static __always_inline void rx_finish(struct cham_ebpf_ctx *ctx,
    struct homa_rpc *rpc);
static __always_inline int grant_poll(struct cham_ebpf_ctx *ctx);
static __always_inline int rx_handle_grant(struct cham_ebpf_ctx *ctx);

int homa_fast_event_rx(struct cham_ebpf_ctx *ctx)
{
  struct homa_pkt *p = ctx->pkt;

  if (!rx_is_common_valid(p, ctx->pkt_end))
    return -1;

  switch (p->data.common.type)
  {
    case HOMA_DATA:
      return rx_handle_data(ctx);
    case HOMA_GRANT:
      return rx_handle_grant(ctx);
    case HOMA_ACK:
      return rx_handle_ack(ctx);
    case HOMA_NEED_ACK:
      return rx_handle_need_ack(ctx);
    default:
      return -1;
  }
}

int homa_fast_event_sched(struct cham_ebpf_ctx *ctx)
{
  struct cham_sched_entry *entry;
  struct homa_rpc *rpc;
  struct homa_socket *sock;
  __u32 slot;

  entry = ebpf_sched_head(&ctx->sched, sizeof(*entry));
  if (entry == NULL || entry->priority > ebpf_now_us())
    return 0;

  slot = entry->id;
  sched_pop(&ctx->sched);
  if (slot == HOMA_GRANT_SCHED_ID)
    return grant_poll(ctx);

  rpc = rpc_at(ctx, slot);
  if (rpc == NULL || rpc->state == HOMA_RPC_FREE)
    return 0;

  sock = sock_at(ctx, rpc->sock_id);
  if (sock == NULL)
    return 0;

  if (rpc->flags & HOMA_RPC_TX_PENDING)
    return tx_segment(ctx, rpc, slot);

  if (rpc->state == HOMA_RPC_RESPONSE_SENT)
  {
    rpc_schedule(ctx, slot, HOMA_NEED_ACK_US);
    return tx_control(ctx, sock, rpc->remote_ip, rpc->remote_port,
        rpc->id, HOMA_NEED_ACK);
  }

  return 0;
}

int homa_fast_event_deq(struct cham_ebpf_ctx *ctx)
{
  struct homa_queue_entry *qe = (struct homa_queue_entry *) ctx->qe;
  struct homa_rpc *rpc;
  struct homa_socket *sock;
  __u32 slot;
  __u64 handle;

  if ((__u8 *) qe + sizeof(*qe) > (__u8 *) ctx->shm_end)
    return -1;

  if (qe->type == HOMA_QUEUE_CLOSE_START)
  {
    sock = sock_at(ctx, qe->data.request.sock_id);
    if (sock != NULL && sock->state != HOMA_SOCKET_FREE)
      sock->state = HOMA_SOCKET_CLOSING;

    return 0;
  }

  if (qe->type == HOMA_QUEUE_CLOSE_DONE)
  {
    sock = sock_at(ctx, qe->data.request.sock_id);
    if (sock != NULL && sock->state == HOMA_SOCKET_CLOSING)
      sock->state = HOMA_SOCKET_CLOSED;

    return 0;
  }

  handle = qe->data.rpc_handle;
  if (handle == 0)
    return 0;

  slot = (handle - 1) % HOMA_MAX_RPCS;
  rpc = rpc_at(ctx, slot);
  if (rpc == NULL || rpc->handle != handle || rpc->state == HOMA_RPC_FREE)
    return 0;

  switch (qe->type)
  {
    case HOMA_QUEUE_TX:
      return deq_handle_tx(ctx, rpc, slot);
    case HOMA_QUEUE_RELEASE:
      rpc->flags |= HOMA_RPC_RX_RELEASED;
      rpc->rx_len = 0;
      rpc_reclaim(ctx, rpc, slot);
      return 0;
    case HOMA_QUEUE_REAP:
      rpc_free(ctx, rpc, slot);
      return 0;
    default:
      return -1;
  }
}

static __always_inline struct homa_rpc *rpc_at(struct cham_ebpf_ctx *ctx,
    __u32 slot)
{
  if (slot >= HOMA_MAX_RPCS)
    return NULL;

  return ebpf_map_lookup(ctx->maps[HOMA_RPC_MAP].addr, slot,
      sizeof(struct homa_rpc));
}

static __always_inline struct homa_socket *sock_at(struct cham_ebpf_ctx *ctx,
    __u32 sock_id)
{
  if (sock_id >= HOMA_MAX_SOCKETS)
    return NULL;

  return ebpf_map_lookup(ctx->maps[HOMA_SOCKET_MAP].addr, sock_id,
      sizeof(struct homa_socket));
}

static __always_inline struct homa_state *state_get(struct cham_ebpf_ctx *ctx)
{
  return ebpf_map_lookup(ctx->maps[HOMA_STATE_MAP].addr, 0,
      sizeof(struct homa_state));
}

static __always_inline __u32 rpc_hash(__u64 id, __u32 sock_id,
    __u32 remote_ip, __u16 remote_port)
{
  __u64 client_id = id / HOMA_RPC_ID_STRIDE;
  __u32 hash = client_id ^ (client_id >> 32) ^ remote_ip ^
      (remote_ip >> 16) ^ remote_port ^ sock_id;

  return hash & (HOMA_CLIENT_RPCS - 1);
}

static __always_inline struct homa_rpc *rpc_server_find(
    struct cham_ebpf_ctx *ctx, __u64 id, __u32 sock_id, __u32 remote_ip,
    __u16 remote_port, __u32 *slot)
{
  struct homa_state *state = state_get(ctx);
  struct homa_rpc *rpc;
  __u32 link, i;

  if (state == NULL)
    return NULL;

  link = state->server_buckets[rpc_hash(id, sock_id, remote_ip, remote_port)];
  for (i = 0; i < HOMA_CLIENT_RPCS; i++)
  {
    if (link <= HOMA_CLIENT_RPCS || link > HOMA_MAX_RPCS)
      return NULL;

    rpc = rpc_at(ctx, link - 1);
    if (rpc == NULL)
      return NULL;

    if (rpc->id == id && rpc->sock_id == sock_id &&
        rpc->remote_ip == remote_ip && rpc->remote_port == remote_port)
    {
      *slot = link - 1;
      return rpc;
    }

    link = rpc->hash_next;
  }

  return NULL;
}

static __always_inline void rpc_server_insert(struct cham_ebpf_ctx *ctx,
    struct homa_rpc *rpc, __u32 slot)
{
  struct homa_state *state = state_get(ctx);
  struct homa_rpc *next;
  __u32 bucket;

  if (state == NULL)
    return;

  bucket = rpc_hash(rpc->id, rpc->sock_id, rpc->remote_ip, rpc->remote_port);
  rpc->hash_prev = 0;
  rpc->hash_next = state->server_buckets[bucket];
  if (rpc->hash_next != 0)
  {
    next = rpc_at(ctx, rpc->hash_next - 1);
    if (next != NULL)
      next->hash_prev = slot + 1;
  }

  state->server_buckets[bucket] = slot + 1;
}

static __always_inline void rpc_server_remove(struct cham_ebpf_ctx *ctx,
    struct homa_rpc *rpc)
{
  struct homa_state *state = state_get(ctx);
  struct homa_rpc *neighbor;
  __u32 bucket;

  if (state == NULL)
    return;

  if (rpc->hash_prev != 0)
  {
    neighbor = rpc_at(ctx, rpc->hash_prev - 1);
    if (neighbor != NULL)
      neighbor->hash_next = rpc->hash_next;
  }
  else
  {
    bucket = rpc_hash(rpc->id, rpc->sock_id, rpc->remote_ip, rpc->remote_port);
    state->server_buckets[bucket] = rpc->hash_next;
  }

  if (rpc->hash_next != 0)
  {
    neighbor = rpc_at(ctx, rpc->hash_next - 1);
    if (neighbor != NULL)
      neighbor->hash_prev = rpc->hash_prev;
  }
}

static __always_inline void rpc_free(struct cham_ebpf_ctx *ctx,
    struct homa_rpc *rpc, __u32 slot)
{
  struct homa_state *state;

  sched_remove(&ctx->sched, slot);
  rx_finish(ctx, rpc);

  if (rpc_is_server(rpc->id))
  {
    rpc_server_remove(ctx, rpc);
    state = state_get(ctx);
    if (state != NULL && state->server_free_count < HOMA_CLIENT_RPCS)
      state->server_free[state->server_free_count++] = slot;
  }

  rpc->flags = 0;
  rpc->rx_len = 0;
  rpc->tx_len = 0;
  MEM_BARRIER();
  rpc->state = HOMA_RPC_FREE;
}

static __always_inline void rpc_reclaim(struct cham_ebpf_ctx *ctx,
    struct homa_rpc *rpc, __u32 slot)
{
  __u8 released = (rpc->flags & HOMA_RPC_RX_RELEASED) != 0;
  __u8 client_done = rpc->state == HOMA_RPC_RESPONSE_READY;
  __u8 server_done = rpc->state == HOMA_RPC_RESPONSE_ACKED;

  if (released && (client_done || server_done))
    rpc_free(ctx, rpc, slot);
}

static __always_inline void rpc_schedule(struct cham_ebpf_ctx *ctx,
    __u32 slot, __u32 delay_us)
{
  sched_remove(&ctx->sched, slot);
  sched_add(&ctx->sched, slot, ebpf_now_us() + delay_us, 1);
}

static __always_inline void rx_ack_server(struct cham_ebpf_ctx *ctx,
    __u64 client_id, __u32 remote_ip, __u16 client_port, __u16 server_port)
{
  struct homa_rpc *rpc;
  __u32 *port, slot;

  if (client_id == 0 || rpc_is_server(client_id))
    return;

  port = ebpf_map_lookup(ctx->maps[HOMA_PORT_MAP].addr, server_port,
      sizeof(*port));
  if (port == NULL || *port == 0 || *port > HOMA_MAX_SOCKETS)
    return;

  rpc = rpc_server_find(ctx, client_id ^ HOMA_RPC_SERVER_BIT, *port - 1,
      remote_ip, client_port, &slot);
  if (rpc == NULL || rpc->state != HOMA_RPC_RESPONSE_SENT)
    return;

  rpc->state = HOMA_RPC_RESPONSE_ACKED;
  rpc->tx_len = 0;
  sched_remove(&ctx->sched, slot);
  rpc_reclaim(ctx, rpc, slot);
}

static __always_inline struct homa_rpc *rx_rpc_lookup(
    struct cham_ebpf_ctx *ctx, __u64 id, __u32 sock_id, __u32 remote_ip,
    __u16 remote_port, __u32 *slot)
{
  struct homa_rpc *rpc;
  struct homa_state *state;
  __u32 i, count;

  if (!rpc_is_server(id))
  {
    if (id < HOMA_RPC_ID_STRIDE)
      return NULL;

    i = rpc_get_client_slot(id);
    if (i >= HOMA_CLIENT_RPCS)
      return NULL;

    rpc = rpc_at(ctx, i);
    if (!rpc_is_response_match(rpc, id, sock_id, remote_ip, remote_port))
      return NULL;

    *slot = i;
    return rpc;
  }

  rpc = rpc_server_find(ctx, id, sock_id, remote_ip, remote_port, slot);
  if (rpc != NULL)
    return rpc->state == HOMA_RPC_REQUEST_RECEIVING ? rpc : NULL;

  state = state_get(ctx);
  if (state == NULL)
    return NULL;

  count = state->server_free_count;
  if (count == 0 || count > HOMA_CLIENT_RPCS)
    return NULL;

  i = state->server_free[(count - 1) & (HOMA_CLIENT_RPCS - 1)];
  if (i < HOMA_CLIENT_RPCS || i >= HOMA_MAX_RPCS)
    return NULL;

  rpc = rpc_at(ctx, i);
  if (rpc == NULL)
    return NULL;

  state->server_free_count = count - 1;
  if (rpc->handle == 0)
    rpc->handle = i + 1;
  else
    rpc->handle += HOMA_MAX_RPCS;

  rpc->id = id;
  rpc->sock_id = sock_id;
  rpc->remote_ip = remote_ip;
  rpc->remote_port = remote_port;
  rpc->flags = 0;
  rpc->tx_len = 0;
  rpc->rx_len = 0;
  __builtin_memset(rpc->rx_bitmap, 0, sizeof(rpc->rx_bitmap));
  rpc->state = HOMA_RPC_REQUEST_RECEIVING;
  rpc->hash_prev = 0;
  rpc->hash_next = 0;
  rpc_server_insert(ctx, rpc, i);
  *slot = i;
  return rpc;
}

static __always_inline int rx_handle_data(struct cham_ebpf_ctx *ctx)
{
  struct homa_pkt *p = ctx->pkt;
  struct homa_socket *sock;
  struct homa_rpc *rpc;
  struct homa_queue_entry *qe;
  struct equeue *q;
  struct homa_state *state;
  __u32 sock_id, slot, len, offset, seg_len, bit, word, index;
  __u64 id;

  if (!rx_is_pkt_valid(p, ctx->pkt_end))
    return -1;

  len = f_beui32(p->data.message_length);
  sock = sock_find(ctx, p, &sock_id);
  if (sock == NULL)
    return -1;

  /* Reserve the notification before admitting a new RPC. */
  if (sock->app_qid >= MAX_PROTO_QUEUES)
    return -1;

  q = &ctx->equeues[sock->app_qid].eq;
  qe = ebpf_queue_tail(q, sizeof(*qe));
  if (qe == NULL)
    return -1;

  id = rx_get_local_rpc_id(p);
  rpc = rx_rpc_lookup(ctx, id, sock_id, f_beui32(p->ip.src),
      f_beui16(p->data.common.sport), &slot);
  if (rpc == NULL)
    return 0;

  if (rpc_is_server(id))
    rpc->app_qid = sock->app_qid;
  else if (rpc->app_qid != sock->app_qid)
    return 0;

  offset = f_beui32(p->data.offset);
  seg_len = f_beui32(p->data.segment_length);
  state = state_get(ctx);
  if (state == NULL)
    return -1;

  if (!(rpc->flags & HOMA_RPC_RX_ACTIVE))
  {
    rpc->rx_len = len;
    rpc->rx_received = 0;
    rpc->rx_credit = f_beui32(p->data.incoming);
    rpc->flags |= HOMA_RPC_RX_ACTIVE;
    state->incoming += rpc->rx_credit;
    if (rpc->rx_credit < len)
      grant_append(ctx, rpc, slot);
  }

  if (rpc->rx_len != len || offset + seg_len > rpc->rx_credit)
    return -1;

  index = offset / HOMA_MAX_PAYLOAD;
  word = index / 32;
  bit = 1U << (index % 32);
  if (word >= HOMA_BITMAP_WORDS)
    return -1;

  if (rpc->rx_bitmap[word] & bit)
    return 0;

  if (offset > HOMA_MESSAGE_BYTES || seg_len > HOMA_MESSAGE_BYTES - offset)
    return -1;

  ebpf_memcpy(rpc->rx_data + offset, (__u8 *) p + sizeof(*p), seg_len);
  rpc->rx_bitmap[word] |= bit;
  rpc->rx_received += seg_len;
  state->incoming -= seg_len;
  grant_wake(ctx);
  if (rpc->rx_received != len)
    return 0;

  rx_finish(ctx, rpc);
  rpc->state = rpc_is_server(id) ?
      HOMA_RPC_REQUEST_READY : HOMA_RPC_RESPONSE_READY;
  qe->data.rpc_handle = rpc->handle;
  queue_enqueue(q, HOMA_QUEUE_RX);

  if (!rpc_is_server(id))
  {
    rpc->tx_len = 0;
    return tx_control(ctx, sock, rpc->remote_ip, rpc->remote_port,
        rpc->id, HOMA_ACK);
  }

  return 0;
}

static __always_inline int rx_handle_ack(struct cham_ebpf_ctx *ctx)
{
  struct homa_pkt *p = ctx->pkt;
  struct homa_ack_hdr *ack = (void *) &p->data;
  __u32 len = sizeof(p->ip) + sizeof(*ack);

  if ((__u8 *) ctx->pkt + len > (__u8 *) ctx->pkt_end ||
      f_beui16(p->ip.len) != len || f_beui16(ack->num_acks) != 0)
    return -1;

  rx_ack_server(ctx, f_beui64(ack->common.sender_id), f_beui32(p->ip.src),
      f_beui16(ack->common.sport), f_beui16(ack->common.dport));
  return 0;
}

static __always_inline int rx_handle_need_ack(struct cham_ebpf_ctx *ctx)
{
  struct homa_pkt *p = ctx->pkt;
  struct homa_socket *sock;
  struct homa_rpc *rpc;
  __u32 sock_id, slot, remote_ip;
  __u16 remote_port;
  __u64 id;

  if (f_beui16(p->ip.len) != sizeof(p->ip) + sizeof(p->data.common))
    return -1;

  sock = sock_find(ctx, p, &sock_id);
  if (sock == NULL)
    return 0;

  id = rx_get_local_rpc_id(p);
  if (rpc_is_server(id) || id == 0)
    return 0;

  remote_ip = f_beui32(p->ip.src);
  remote_port = f_beui16(p->data.common.sport);
  slot = rpc_get_client_slot(id);
  rpc = slot < HOMA_CLIENT_RPCS ? rpc_at(ctx, slot) : NULL;
  if (rpc != NULL && rpc->id == id && rpc->state != HOMA_RPC_FREE)
  {
    if (rpc->sock_id != sock_id || rpc->remote_ip != remote_ip ||
        rpc->remote_port != remote_port ||
        rpc->state == HOMA_RPC_WAIT_RESPONSE)
      return 0;
  }

  return tx_control(ctx, sock, remote_ip, remote_port, id, HOMA_ACK);
}

static __always_inline int deq_handle_tx(struct cham_ebpf_ctx *ctx,
    struct homa_rpc *rpc, __u32 slot)
{
  if (!rpc_is_tx_ready(rpc) || (rpc->flags & HOMA_RPC_TX_PENDING))
    return 0;

  rpc->tx_offset = 0;
  rpc->tx_credit = tx_initial_credit(ctx, rpc->tx_len);
  rpc->flags |= HOMA_RPC_TX_PENDING;
  return tx_segment(ctx, rpc, slot);
}

static __always_inline int tx_segment(struct cham_ebpf_ctx *ctx,
    struct homa_rpc *rpc, __u32 slot)
{
  struct homa_pkt *p = ctx->pkt;
  struct homa_socket *sock;
  __u32 offset = rpc->tx_offset;
  __u32 len;

  sock = sock_at(ctx, rpc->sock_id);
  if (sock == NULL || sock->state != HOMA_SOCKET_BOUND)
    return 0;

  if (rpc->tx_len > HOMA_MESSAGE_BYTES || offset > rpc->tx_len)
    return -1;

  len = rpc->tx_len - offset;
  if (len > HOMA_MAX_PAYLOAD)
    len = HOMA_MAX_PAYLOAD;

  if (offset + len > rpc->tx_credit)
    return 0;

  if (!is_paylen_valid(p, ctx->pkt_end, len))
    return -1;

  if (offset > HOMA_MESSAGE_BYTES || len > HOMA_MESSAGE_BYTES - offset)
    return -1;

  tx_fill_ip_hdr(&p->ip, sock->local_ip, rpc->remote_ip, sizeof(*p) + len);
  tx_fill_data_hdr(&p->data, sock, rpc, len);
  ebpf_memcpy((__u8 *) p + sizeof(*p), rpc->tx_data + offset, len);

  rpc->tx_offset = offset + len;
  if (rpc->tx_offset < rpc->tx_len)
  {
    if (rpc->tx_offset < rpc->tx_credit)
      rpc_schedule(ctx, slot, 0);
  }
  else
  {
    rpc->flags &= ~HOMA_RPC_TX_PENDING;
    if (rpc_is_server(rpc->id))
    {
      rpc->state = HOMA_RPC_RESPONSE_SENT;
      rpc_schedule(ctx, slot, HOMA_NEED_ACK_US);
    }
  }

  return sizeof(*p) + len;
}

static __always_inline int tx_control(struct cham_ebpf_ctx *ctx,
    struct homa_socket *sock, __u32 remote_ip, __u16 remote_port,
    __u64 id, __u8 type)
{
  struct homa_pkt *p = ctx->pkt;
  struct homa_ack_hdr *ack = (void *) &p->data;
  __u32 hdrlen = type == HOMA_ACK ? sizeof(*ack) : sizeof(ack->common);
  __u32 len = sizeof(p->ip) + hdrlen;

  if ((__u8 *) p + len > (__u8 *) ctx->pkt_end)
    return -1;

  tx_fill_ip_hdr(&p->ip, sock->local_ip, remote_ip, len);
  tx_fill_common_hdr(&ack->common, sock->local_port, remote_port, id, type);
  if (type == HOMA_ACK)
  {
    ack->num_acks = t_beui16(0);
    __builtin_memset(ack->acks, 0, sizeof(ack->acks));
  }

  return len;
}

static __always_inline __u8 rx_is_common_valid(struct homa_pkt *p,
    void *pkt_end)
{
  __u32 len = sizeof(p->ip) + sizeof(p->data.common);
  __u8 ipv4, fixed_ip_hdr, homa, unfragmented;

  if ((__u8 *) p + len > (__u8 *) pkt_end)
    return 0;

  ipv4 = IPH_V(&p->ip) == 4;
  fixed_ip_hdr = IPH_HL(&p->ip) == sizeof(p->ip) / sizeof(__u32);
  homa = p->ip.proto == IP_PROTO_HOMA;
  unfragmented = !(f_beui16(p->ip.offset) & IPV4_FRAGMENT_MASK);
  return ipv4 && fixed_ip_hdr && homa && unfragmented;
}

static __always_inline void tx_fill_ip_hdr(struct ip_hdr *ip,
    __u32 local_ip, __u32 remote_ip, __u32 len)
{
  IPH_VHL_SET(ip, 4, sizeof(*ip) / sizeof(__u32));
  ip->_tos = 0;
  ip->len = t_beui16(len);
  ip->id = t_beui16(0);
  ip->offset = t_beui16(0);
  ip->ttl = HOMA_IP_TTL;
  ip->proto = IP_PROTO_HOMA;
  ip->chksum = 0;
  ip->src = t_beui32(local_ip);
  ip->dst = t_beui32(remote_ip);
}

static __always_inline void tx_fill_common_hdr(struct homa_common_hdr *common,
    __u16 local_port, __u16 remote_port, __u64 id, __u8 type)
{
  common->sport = t_beui16(local_port);
  common->dport = t_beui16(remote_port);
  common->unused[0] = 0;
  common->unused[1] = 0;
  common->doff = 0;
  common->type = type;
  common->seq = 0;
  common->checksum = 0;
  common->unused4 = 0;
  common->sender_id = t_beui64(id);
}

static __always_inline __u8 is_paylen_valid(struct homa_pkt *p,
    void *pkt_end, __u32 len)
{
  __u8 single_packet = len <= HOMA_MAX_PAYLOAD;
  __u8 in_bounds = (__u8 *) p + sizeof(*p) + len <= (__u8 *) pkt_end;

  return single_packet && in_bounds;
}

static __always_inline __u8 rx_is_pkt_valid(struct homa_pkt *p, void *pkt_end)
{
  __u32 len, offset, seg_len, remaining;
  __u8 data_pkt, data_hdr, segment_valid, credit_valid, ip_len_valid;

  if ((__u8 *) p + sizeof(*p) > (__u8 *) pkt_end)
    return 0;

  len = f_beui32(p->data.message_length);
  data_pkt = p->data.common.type == HOMA_DATA;
  data_hdr = p->data.common.doff == ((sizeof(p->data) / sizeof(__u32)) << 4);
  offset = f_beui32(p->data.offset);
  seg_len = f_beui32(p->data.segment_length);
  if (len > HOMA_MESSAGE_BYTES || offset > len)
    return 0;

  if (offset % HOMA_MAX_PAYLOAD != 0 || (len != 0 && offset == len))
    return 0;

  remaining = len - offset;
  segment_valid = seg_len == (remaining > HOMA_MAX_PAYLOAD ?
      HOMA_MAX_PAYLOAD : remaining);
  credit_valid = f_beui32(p->data.incoming) <= len &&
      f_beui32(p->data.incoming) >= offset + seg_len;
  ip_len_valid = f_beui16(p->ip.len) == sizeof(*p) + seg_len;

  return data_pkt && data_hdr && segment_valid && credit_valid &&
      ip_len_valid && is_paylen_valid(p, pkt_end, seg_len);
}

static __always_inline __u64 rx_get_local_rpc_id(struct homa_pkt *p)
{
  /* The receiver uses the opposite role bit for the same RPC. */
  return f_beui64(p->data.common.sender_id) ^ HOMA_RPC_SERVER_BIT;
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

  return (client_handle - 1) % HOMA_MAX_RPCS;
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

static __always_inline void tx_fill_data_hdr(struct homa_data_hdr *data,
    struct homa_socket *sock, struct homa_rpc *rpc, __u32 len)
{
  tx_fill_common_hdr(&data->common, sock->local_port, rpc->remote_port,
      rpc->id, HOMA_DATA);
  data->common.doff = (sizeof(*data) / sizeof(__u32)) << 4;
  data->message_length = t_beui32(rpc->tx_len);
  data->incoming = t_beui32(rpc->tx_credit);
  data->cutoff_version = t_beui16(0);
  data->retransmit = 0;
  data->unused = 0;
  data->offset = t_beui32(rpc->tx_offset);
  data->segment_length = t_beui32(len);
  data->ack_id = t_beui64(0);
  data->ack_sport = t_beui16(0);
  data->ack_dport = t_beui16(0);
}

static __always_inline __u32 tx_initial_credit(struct cham_ebpf_ctx *ctx,
    __u32 len)
{
  struct homa_state *state = state_get(ctx);
  __u32 credit;

  if (state == NULL)
    return 0;

  credit = state->unscheduled_bytes;
  if (len <= credit)
    return len;

  return credit / HOMA_MAX_PAYLOAD * HOMA_MAX_PAYLOAD;
}

static __always_inline void grant_wake(struct cham_ebpf_ctx *ctx)
{
  struct homa_state *state = state_get(ctx);

  if (state != NULL && state->grant_head != 0)
    rpc_schedule(ctx, HOMA_GRANT_SCHED_ID, 0);
}

static __always_inline void grant_append(struct cham_ebpf_ctx *ctx,
    struct homa_rpc *rpc, __u32 slot)
{
  struct homa_state *state = state_get(ctx);
  struct homa_rpc *tail;

  if (state == NULL)
    return;

  rpc->grant_prev = state->grant_tail;
  rpc->grant_next = 0;
  if (state->grant_tail != 0)
  {
    tail = rpc_at(ctx, state->grant_tail - 1);
    if (tail != NULL)
      tail->grant_next = slot + 1;
  }
  else
  {
    state->grant_head = slot + 1;
  }

  state->grant_tail = slot + 1;
  rpc->flags |= HOMA_RPC_GRANT_QUEUED;
}

static __always_inline void grant_remove(struct cham_ebpf_ctx *ctx,
    struct homa_rpc *rpc)
{
  struct homa_state *state = state_get(ctx);
  struct homa_rpc *neighbor;

  if (state == NULL || !(rpc->flags & HOMA_RPC_GRANT_QUEUED))
    return;

  if (rpc->grant_prev != 0)
  {
    neighbor = rpc_at(ctx, rpc->grant_prev - 1);
    if (neighbor != NULL)
      neighbor->grant_next = rpc->grant_next;
  }
  else
  {
    state->grant_head = rpc->grant_next;
  }

  if (rpc->grant_next != 0)
  {
    neighbor = rpc_at(ctx, rpc->grant_next - 1);
    if (neighbor != NULL)
      neighbor->grant_prev = rpc->grant_prev;
  }
  else
  {
    state->grant_tail = rpc->grant_prev;
  }

  rpc->flags &= ~HOMA_RPC_GRANT_QUEUED;
}

static __always_inline void rx_finish(struct cham_ebpf_ctx *ctx,
    struct homa_rpc *rpc)
{
  struct homa_state *state = state_get(ctx);

  if (state == NULL || !(rpc->flags & HOMA_RPC_RX_ACTIVE))
    return;

  state->incoming -= rpc->rx_credit - rpc->rx_received;
  rpc->flags &= ~HOMA_RPC_RX_ACTIVE;
  grant_remove(ctx, rpc);
  grant_wake(ctx);
}

static __always_inline int grant_poll(struct cham_ebpf_ctx *ctx)
{
  struct homa_state *state = state_get(ctx);
  struct homa_grant_hdr *grant;
  struct homa_pkt *p = ctx->pkt;
  struct homa_socket *sock;
  struct homa_rpc *rpc;
  __u32 target, available;
  __u32 len = sizeof(p->ip) + sizeof(*grant);

  if (state == NULL || state->grant_head == 0)
    return 0;

  rpc = rpc_at(ctx, state->grant_head - 1);
  if (rpc == NULL || !(rpc->flags & HOMA_RPC_RX_ACTIVE))
    return 0;

  sock = sock_at(ctx, rpc->sock_id);
  if (sock == NULL || sock->state != HOMA_SOCKET_BOUND)
    return 0;

  if (state->incoming >= state->incoming_limit)
    return 0;

  available = state->incoming_limit - state->incoming;
  target = rpc->rx_received + state->grant_bytes;
  if (target > rpc->rx_len)
    target = rpc->rx_len;

  if (target <= rpc->rx_credit)
    return 0;

  if (target - rpc->rx_credit > available)
    target = rpc->rx_credit + available;

  if (target < rpc->rx_len)
    target = target / HOMA_MAX_PAYLOAD * HOMA_MAX_PAYLOAD;

  if (target <= rpc->rx_credit)
    return 0;

  if ((__u8 *) p + len > (__u8 *) ctx->pkt_end)
    return -1;

  grant = (void *) &p->data;
  tx_fill_ip_hdr(&p->ip, sock->local_ip, rpc->remote_ip, len);
  tx_fill_common_hdr(&grant->common, sock->local_port, rpc->remote_port,
      rpc->id, HOMA_GRANT);
  grant->offset = t_beui32(target);
  grant->priority = HOMA_GRANT_PRIORITY;
  grant->resend_all = 0;

  state->incoming += target - rpc->rx_credit;
  rpc->rx_credit = target;
  return len;
}

static __always_inline int rx_handle_grant(struct cham_ebpf_ctx *ctx)
{
  struct homa_pkt *p = ctx->pkt;
  struct homa_grant_hdr *grant = (void *) &p->data;
  struct homa_socket *sock;
  struct homa_rpc *rpc;
  __u32 sock_id, slot, offset, remote_ip;
  __u16 remote_port;
  __u64 id;
  __u32 len = sizeof(p->ip) + sizeof(*grant);

  if ((__u8 *) p + len > (__u8 *) ctx->pkt_end ||
      f_beui16(p->ip.len) != len || grant->resend_all != 0)
    return -1;

  sock = sock_find(ctx, p, &sock_id);
  if (sock == NULL)
    return 0;

  id = rx_get_local_rpc_id(p);
  remote_ip = f_beui32(p->ip.src);
  remote_port = f_beui16(grant->common.sport);
  if (rpc_is_server(id))
  {
    rpc = rpc_server_find(ctx, id, sock_id, remote_ip, remote_port, &slot);
  }
  else
  {
    if (id < HOMA_RPC_ID_STRIDE)
      return 0;

    slot = rpc_get_client_slot(id);
    rpc = rpc_at(ctx, slot);
    if (!rpc_is_response_match(rpc, id, sock_id, remote_ip, remote_port))
      return 0;
  }

  if (!rpc_is_tx_ready(rpc) || !(rpc->flags & HOMA_RPC_TX_PENDING))
    return 0;

  offset = f_beui32(grant->offset);
  if (offset <= rpc->tx_credit || offset > rpc->tx_len)
    return 0;

  if (offset != rpc->tx_len && offset % HOMA_MAX_PAYLOAD != 0)
    return -1;

  rpc->tx_credit = offset;
  rpc_schedule(ctx, slot, 0);
  return 0;
}

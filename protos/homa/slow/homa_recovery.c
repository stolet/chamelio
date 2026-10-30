#include "clock.h"
#include "homa_hdr.h"
#include "homa_slow.h"
#include "queue_fns.h"

#define NS_PER_US 1000ULL

static int is_restart_allowed(struct homa_rpc *rpc);
static int is_resend_valid(struct homa_slow *ctx,
    struct homa_queue_packet *packet);
static int is_tx_delayed(struct homa_rpc *rpc);
static int is_rpc_match(struct homa_rpc *rpc,
    struct homa_queue_packet *packet, __u32 sock_id);

static __u8 packet_handle(struct homa_slow *ctx,
    struct homa_queue_entry *packet, struct homa_queue_entry *command);
static struct homa_rpc *rpc_find(struct homa_slow *ctx,
    struct homa_queue_packet *packet, __u32 sock_id,
    __u64 *handle, __u64 *progress);
static void command_init(struct homa_queue_recovery *command,
    __u64 handle, __u64 progress);
static void timer_reset(struct homa_slow *ctx, __u64 handle, __u64 progress);

void homa_recovery_poll(struct homa_slow *ctx)
{
  struct homa_queue_entry *packet, *command;
  __u32 i;
  __u8 type;

  for (i = 0; i < HOMA_QUEUE_LENGTH; i++)
  {
    packet = queue_head(&ctx->fast_slow);
    if (packet == NULL)
      break;

    command = queue_tail(&ctx->slow_fast);
    if (command == NULL)
      break;

    type = packet_handle(ctx, packet, command);
    if (type != HOMA_QUEUE_EMPTY)
      queue_enqueue(&ctx->slow_fast, type);
    queue_dequeue(&ctx->fast_slow);
  }
}

static __u8 packet_handle(struct homa_slow *ctx,
    struct homa_queue_entry *entry, struct homa_queue_entry *command)
{
  struct homa_queue_packet *packet = &entry->data.packet;
  struct homa_queue_recovery *recovery = &command->data.recovery;
  struct homa_socket *sock;
  struct homa_rpc *rpc;
  __u32 sock_id, end;
  __u64 handle, progress;

  sock_id = ctx->ports[packet->local_port];
  if (sock_id == 0 || sock_id > HOMA_MAX_SOCKETS)
    return HOMA_QUEUE_EMPTY;

  sock_id--;
  sock = &ctx->sockets[sock_id];
  if (sock->state != HOMA_SOCKET_BOUND || sock->local_ip != packet->local_ip)
    return HOMA_QUEUE_EMPTY;

  command->data.packet = *packet;
  command->data.packet.sock_id = sock_id;
  command->data.packet.type = HOMA_BUSY;
  rpc = rpc_find(ctx, packet, sock_id, &handle, &progress);
  if (rpc == NULL)
  {
    if (entry->type != HOMA_QUEUE_RX_RESEND)
      return HOMA_QUEUE_EMPTY;

    command->data.packet.type = HOMA_UNKNOWN;
    return HOMA_QUEUE_CONTROL;
  }

  if (entry->type == HOMA_QUEUE_RX_BUSY)
  {
    timer_reset(ctx, handle, progress);
    return HOMA_QUEUE_EMPTY;
  }

  if (entry->type == HOMA_QUEUE_RX_UNKNOWN)
  {
    if (rpc->id & HOMA_RPC_SERVER_BIT)
    {
      command_init(recovery, handle, progress);
      return HOMA_QUEUE_ABORT;
    }

    if (!is_restart_allowed(rpc))
      return HOMA_QUEUE_EMPTY;

    command_init(recovery, handle, progress);
    recovery->length = rpc->tx_offset;
    recovery->credit = rpc->tx_credit;
    return HOMA_QUEUE_RETRANSMIT;
  }

  if (!is_resend_valid(ctx, packet))
    return HOMA_QUEUE_EMPTY;

  timer_reset(ctx, handle, progress);
  if (packet->length == 0)
    return HOMA_QUEUE_CONTROL;

  if ((rpc->id & HOMA_RPC_SERVER_BIT) &&
      !(rpc->flags & HOMA_RPC_TX_STARTED))
  {
    if ((rpc->flags & HOMA_RPC_RX_ACTIVE) &&
        rpc->rx_received < rpc->rx_credit)
      return HOMA_QUEUE_EMPTY;
    return HOMA_QUEUE_CONTROL;
  }

  if (is_tx_delayed(rpc))
    return HOMA_QUEUE_CONTROL;

  end = packet->offset + packet->length;
  if (end < rpc->tx_len)
    end = (end + ctx->payload_bytes - 1) / ctx->payload_bytes *
        ctx->payload_bytes;
  if (end > rpc->tx_len)
    end = rpc->tx_len;

  command_init(recovery, handle, progress);
  recovery->credit = end;
  recovery->priority = packet->priority;
  if (packet->offset >= rpc->tx_offset && rpc->tx_len != 0)
    return HOMA_QUEUE_CREDIT;

  if (end > rpc->tx_offset)
    end = rpc->tx_offset;
  recovery->offset = packet->offset;
  recovery->length = end - packet->offset;
  return HOMA_QUEUE_RETRANSMIT;
}

static struct homa_rpc *rpc_find(struct homa_slow *ctx,
    struct homa_queue_packet *packet, __u32 sock_id,
    __u64 *handle, __u64 *progress)
{
  struct homa_rpc *rpc;
  __u32 begin, end, slot;

  if (packet->id & HOMA_RPC_SERVER_BIT)
  {
    begin = ctx->config.rpc_slots;
    end = ctx->config.rpc_slots + HOMA_SERVER_RPCS;
  }
  else
  {
    if (packet->id < HOMA_RPC_ID_STRIDE)
      return NULL;

    begin = (packet->id / HOMA_RPC_ID_STRIDE - 1) % HOMA_MAX_RPCS;
    if (begin >= ctx->config.rpc_slots)
      return NULL;
    end = begin + 1;
  }

  for (slot = begin; slot < end; slot++)
  {
    rpc = &ctx->rpcs[slot];
    *handle = __atomic_load_n(&rpc->handle, __ATOMIC_ACQUIRE);
    *progress = __atomic_load_n(&rpc->progress, __ATOMIC_ACQUIRE);
    if (rpc->state == HOMA_RPC_FREE || rpc->state == HOMA_RPC_FAILED)
      continue;

    if (is_rpc_match(rpc, packet, sock_id))
      return rpc;
  }

  return NULL;
}

static void command_init(struct homa_queue_recovery *command,
    __u64 handle, __u64 progress)
{
  command->rpc_handle = handle;
  command->progress = progress;
  command->offset = 0;
  command->length = 0;
  command->credit = 0;
  command->priority = HOMA_GRANT_PRIORITY;
}

static void timer_reset(struct homa_slow *ctx, __u64 handle, __u64 progress)
{
  struct homa_rpc_timer *timer =
      &ctx->timers[(handle - 1) % HOMA_MAX_RPCS];

  timer->handle = handle;
  timer->progress = progress;
  timer->attempts = 0;
  timer->deadline = clock_now_ns() + HOMA_RECOVERY_US * NS_PER_US;
}

static int is_restart_allowed(struct homa_rpc *rpc)
{
  int waiting_response = rpc->state == HOMA_RPC_WAIT_RESPONSE;
  int receiving = (rpc->flags & HOMA_RPC_RX_ACTIVE) != 0;
  int started = (rpc->flags & HOMA_RPC_TX_STARTED) != 0;

  return waiting_response && !receiving && started;
}

static int is_resend_valid(struct homa_slow *ctx,
    struct homa_queue_packet *packet)
{
  int length_valid, offset_aligned, priority_valid;

  if (packet->offset > HOMA_MESSAGE_BYTES)
    return 0;

  length_valid = packet->length <= HOMA_MESSAGE_BYTES - packet->offset;
  offset_aligned = packet->offset % ctx->payload_bytes == 0;
  priority_valid = packet->priority <= HOMA_GRANT_PRIORITY;
  return length_valid && offset_aligned && priority_valid;
}

static int is_tx_delayed(struct homa_rpc *rpc)
{
  int completed = rpc->state == HOMA_RPC_RESPONSE_READY ||
      rpc->state == HOMA_RPC_RESPONSE_ACKED;
  int queued = (rpc->flags & HOMA_RPC_TX_QUEUED) != 0;
  int not_started = !(rpc->flags & HOMA_RPC_TX_STARTED);

  return completed || queued || not_started;
}

static int is_rpc_match(struct homa_rpc *rpc,
    struct homa_queue_packet *packet, __u32 sock_id)
{
  int same_id = rpc->id == packet->id;
  int same_socket = rpc->sock_id == sock_id;
  int same_address = rpc->remote_ip == packet->remote_ip;
  int same_port = rpc->remote_port == packet->remote_port;

  return same_id && same_socket && same_address && same_port;
}

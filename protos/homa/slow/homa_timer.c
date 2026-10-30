#include "clock.h"
#include "homa_slow.h"
#include "queue_fns.h"

#define NS_PER_US 1000ULL

static int recovery_range(struct homa_slow *ctx, struct homa_rpc *rpc,
    struct homa_queue_recovery *recovery);
static int is_waiting_local(struct homa_rpc *rpc);
static int is_finished(__u16 state);

void homa_timer_poll(struct homa_slow *ctx)
{
  struct homa_queue_entry *entry;
  struct homa_queue_recovery recovery;
  struct homa_rpc_timer *timer;
  struct homa_rpc *rpc;
  __u64 now = clock_now_ns();
  __u64 handle, progress, interval;
  __u32 slot;
  __u16 state;
  __u8 type;
  int timer_running, new_rpc, progressed;

  for (slot = 0; slot < ctx->config.rpc_slots + HOMA_SERVER_RPCS; slot++)
  {
    rpc = &ctx->rpcs[slot];
    timer = &ctx->timers[slot];
    handle = __atomic_load_n(&rpc->handle, __ATOMIC_ACQUIRE);
    state = __atomic_load_n(&rpc->state, __ATOMIC_ACQUIRE);
    progress = __atomic_load_n(&rpc->progress, __ATOMIC_ACQUIRE);
    if (is_finished(state))
    {
      timer->deadline = 0;
      continue;
    }

    interval = (state == HOMA_RPC_RESPONSE_SENT ?
        HOMA_NEED_ACK_US : HOMA_RECOVERY_US) * NS_PER_US;
    timer_running = timer->deadline != 0;
    new_rpc = timer->handle != handle;
    progressed = timer->progress != progress;
    if (!timer_running || new_rpc || progressed)
    {
      timer->handle = handle;
      timer->progress = progress;
      timer->attempts = 0;
      timer->deadline = now + interval;
      continue;
    }

    if (is_waiting_local(rpc))
    {
      timer->deadline = now + interval;
      continue;
    }

    if (now < timer->deadline)
      continue;

    recovery.rpc_handle = handle;
    recovery.progress = progress;
    recovery.offset = 0;
    recovery.length = 0;
    recovery.credit = 0;
    recovery.priority = HOMA_GRANT_PRIORITY;
    if (timer->attempts >= HOMA_MAX_RETRIES)
      type = HOMA_QUEUE_FAIL;
    else if (state == HOMA_RPC_RESPONSE_SENT)
      type = HOMA_QUEUE_NEED_ACK;
    else
    {
      type = HOMA_QUEUE_RESEND;
      if (!recovery_range(ctx, rpc, &recovery))
        continue;
    }

    entry = queue_tail(&ctx->slow_fast);
    if (entry == NULL)
      return;

    entry->data.recovery = recovery;
    queue_enqueue(&ctx->slow_fast, type);
    timer->attempts++;
    timer->deadline = now + interval;
  }
}

static int recovery_range(struct homa_slow *ctx, struct homa_rpc *rpc,
    struct homa_queue_recovery *recovery)
{
  __u32 index, offset, end, bit, word;
  __u32 credit = rpc->rx_credit;

  if (!(rpc->flags & HOMA_RPC_RX_ACTIVE))
  {
    if (!(rpc->id & HOMA_RPC_SERVER_BIT))
      recovery->length = ctx->payload_bytes;
    return 1;
  }

  if (credit > rpc->rx_len || credit > HOMA_MESSAGE_BYTES)
    return 0;

  for (index = 0; index * ctx->payload_bytes < credit; index++)
  {
    word = index / 32;
    bit = 1U << (index % 32);
    if (rpc->rx_bitmap[word] & bit)
      continue;

    offset = index * ctx->payload_bytes;
    end = offset;
    do
    {
      end += ctx->payload_bytes;
      index++;
      if (end >= credit)
        break;

      word = index / 32;
      bit = 1U << (index % 32);
    }
    while (!(rpc->rx_bitmap[word] & bit));

    if (end > credit)
      end = credit;
    recovery->offset = offset;
    recovery->length = end - offset;
    return 1;
  }

  return 0;
}

static int is_waiting_local(struct homa_rpc *rpc)
{
  int tx_queued = (rpc->flags & HOMA_RPC_TX_QUEUED) != 0;
  int tx_started = (rpc->flags & HOMA_RPC_TX_STARTED) != 0;
  int rx_active = (rpc->flags & HOMA_RPC_RX_ACTIVE) != 0;
  int not_started = !tx_started && !rx_active;
  int waiting_grant = rx_active && rpc->rx_received == rpc->rx_credit &&
      rpc->rx_credit < rpc->rx_len;
  int waiting_reply = rpc->state == HOMA_RPC_REQUEST_READY && !tx_started;

  return tx_queued || not_started || waiting_grant || waiting_reply;
}

static int is_finished(__u16 state)
{
  int inactive = state == HOMA_RPC_FREE || state == HOMA_RPC_FAILED;
  int completed = state == HOMA_RPC_RESPONSE_READY ||
      state == HOMA_RPC_RESPONSE_ACKED;

  return inactive || completed;
}

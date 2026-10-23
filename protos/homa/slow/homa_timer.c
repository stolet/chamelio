#include "clock.h"
#include "homa_slow.h"
#include "queue_fns.h"

#define NS_PER_US 1000ULL

void homa_timer_poll(struct homa_slow *ctx)
{
  struct homa_queue_entry *entry;
  struct homa_rpc *rpc;
  __u64 now = clock_now_ns();
  __u64 handle;
  __u32 slot;

  for (slot = 0; slot < HOMA_CLIENT_RPCS; slot++)
  {
    rpc = &ctx->rpcs[slot + HOMA_CLIENT_RPCS];
    handle = __atomic_load_n(&rpc->handle, __ATOMIC_ACQUIRE);
    if (__atomic_load_n(&rpc->state, __ATOMIC_ACQUIRE) !=
        HOMA_RPC_RESPONSE_SENT)
    {
      ctx->ack_deadlines[slot] = 0;
      continue;
    }

    if (ctx->ack_deadlines[slot] == 0 || ctx->ack_handles[slot] != handle)
    {
      ctx->ack_handles[slot] = handle;
      ctx->ack_deadlines[slot] = now + HOMA_NEED_ACK_US * NS_PER_US;
      continue;
    }

    if (now < ctx->ack_deadlines[slot])
      continue;

    entry = queue_tail(&ctx->slow_fast);
    if (entry == NULL)
      return;

    entry->data.rpc_handle = handle;
    queue_enqueue(&ctx->slow_fast, HOMA_QUEUE_NEED_ACK);
    ctx->ack_deadlines[slot] = now + HOMA_NEED_ACK_US * NS_PER_US;
  }
}

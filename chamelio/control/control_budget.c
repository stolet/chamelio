#include <linux/types.h>
#include <stdint.h>

#include "control_budget.h"
#include "clock.h"

void control_budget_refresh(struct control_context *ctx)
{
  int i;
  __u64 credits, credits_add, credits_guest;

  if (ctx->n_guests == 0)
  {
    ctx->ts_refresh = clock_rdtsc();
    return;
  }

  credits = clock_rdtsc() - ctx->ts_refresh;
  credits_add = credits * ctx->config->perf_iso_boost / ctx->n_guests;
  for (i = 0; i < ctx->n_guests; i++)
  {
    if (ctx->config->perf_iso)
    {
      __atomic_load(&ctx->guests[i].budget, &credits_guest, __ATOMIC_RELAXED);
      if (credits_guest + credits_add > ctx->budget_cap)
        credits_add = ctx->budget_cap - credits_guest;
      __atomic_fetch_add(&ctx->guests[i].budget, credits_add, __ATOMIC_RELAXED);
    }
    else
    {
      __atomic_store_n(&ctx->guests[i].budget, INT64_MAX, __ATOMIC_RELAXED);
    }
  }
  ctx->ts_refresh = clock_rdtsc();
}

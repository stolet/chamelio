#include <linux/types.h>
#include <stdint.h>

#include "control_budget.h"
#include "clock.h"

void control_budget_refresh(struct control_context *ctx)
{
  int i;
  __u64 now_tsc, creds, creds_add;
  __s64 budg, guest_add;

  now_tsc = clock_rdtsc();
  creds = 0;
  if (ctx->ts_refresh != 0)
    creds = now_tsc - ctx->ts_refresh;

#if CHAM_CTL_BUDGET_STATS
  if (ctx->config->perf_iso && ctx->ts_refresh != 0)
  {
    ctx->budg_stats.nr++;
    ctx->budg_stats.cyc += creds;
  }
#endif

  if (ctx->n_guests == 0)
  {
    ctx->ts_refresh = now_tsc;
    return;
  }

  creds_add = creds * ctx->config->perf_iso_boost / ctx->n_guests;
  for (i = 0; i < ctx->n_guests; i++)
  {
    if (ctx->config->perf_iso)
    {
      guest_add = (__s64) creds_add;
      budg = __atomic_load_n(&ctx->guests[i].budget, __ATOMIC_RELAXED);
      if (budg >= (__s64) ctx->budget_cap)
        guest_add = 0;
      else if (budg > (__s64) ctx->budget_cap - guest_add)
        guest_add = (__s64) ctx->budget_cap - budg;

      __atomic_fetch_add(&ctx->guests[i].budget, guest_add, __ATOMIC_RELAXED);
    }
    else
    {
      __atomic_store_n(&ctx->guests[i].budget, INT64_MAX, __ATOMIC_RELAXED);
    }
  }
  ctx->ts_refresh = now_tsc;
}

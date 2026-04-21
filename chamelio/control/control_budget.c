#include <linux/types.h>
#include <stdint.h>

#include "control_budget.h"
#include "clock.h"

static inline __u64 budget_share(__u64 total, __u32 idx, __u32 nr)
{
  __u64 share;

  if (nr == 0)
    return 0;

  share = total / nr;
  if (idx < total % nr)
    share++;

  return share;
}

void control_budget_refresh(struct control_context *ctx)
{
  int i, j;
  __u64 now_tsc, creds, creds_add;
  __u64 budg_cap;
  __s64 budg, guest_add;
  __u32 ncores;

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
  ncores = ctx->config->fp_cores_max;
  for (i = 0; i < ctx->n_guests; i++)
  {
    if (ctx->config->perf_iso)
    {
      for (j = 0; j < (int) ncores; j++)
      {
        budg_cap = budget_share(ctx->budget_cap, j, ncores);
        guest_add = (__s64) budget_share(creds_add, j, ncores);
        budg = __atomic_load_n(&ctx->guests[i].budgets[j].val, __ATOMIC_RELAXED);
        if (budg >= (__s64) budg_cap)
          guest_add = 0;
        else if (budg > (__s64) budg_cap - guest_add)
          guest_add = (__s64) budg_cap - budg;

        __atomic_fetch_add(&ctx->guests[i].budgets[j].val, guest_add,
            __ATOMIC_RELAXED);
      }
    }
    else
    {
      for (j = 0; j < (int) ncores; j++)
      {
        __atomic_store_n(&ctx->guests[i].budgets[j].val, INT64_MAX,
            __ATOMIC_RELAXED);
      }
    }
  }
  ctx->ts_refresh = now_tsc;
}

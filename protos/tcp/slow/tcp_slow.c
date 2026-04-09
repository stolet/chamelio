#include <stdlib.h>

#include "appif.h"
#include "clock.h"
#include "tcp_config.h"
#include "tcp_slow_internal.h"
#include "log.h"

int main(int argc, char **argv)
{
  int ret;
  struct tcp_slow_context ctx;

  ret = tcp_config_parse(&ctx.config, argc, argv);
  if (ret != 0)
  {
    LOG_ERROR("failed to parse TCP configuration");
    abort();
  }

  ret = tcp_slow_init(&ctx);
  if (ret != 0)
  {
    LOG_ERROR("failed to initialise tcp slow context");
    abort();
  }

  ret = appif_init(&ctx);
  if (ret != 0)
  {
    LOG_ERROR("failed to initialise appif");
    abort();
  }

  while (1)
  {
    appif_poll(&ctx);
    tcp_slow_app_poll(&ctx);
    tcp_slow_fast_poll(&ctx);
    tcp_slow_cc_poll(&ctx);
    tcp_slow_timeout_poll(&ctx);

    if (ctx.stats_log_tsc == 0)
    {
      ctx.stats_log_tsc = clock_rdtsc();
    }
    else if (clock_us_since_tsc(ctx.stats_log_tsc) >= 1000000)
    {
      LOG_INFO("slow_stats drops=%llu retransmissions=%llu acks_received=%llu",
          (unsigned long long) ctx.stats_drops,
          (unsigned long long) ctx.stats_retx,
          (unsigned long long) ctx.stats_acks_rx);
      ctx.stats_drops = 0;
      ctx.stats_retx = 0;
      ctx.stats_acks_rx = 0;
      ctx.stats_log_tsc = clock_rdtsc();
    }
  }
}

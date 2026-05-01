#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "app/tcp_appif.h"
#include "clock.h"
#include "tcp_config.h"
#include "tcp_internal.h"
#include "tcp_state.h"
#include "log.h"

static void tcp_state_publish(struct tcp_slow_context *ctx);

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

  ret = tcp_init(&ctx);
  if (ret != 0)
  {
    LOG_ERROR("failed to initialise tcp slow context");
    abort();
  }

  tcp_state_publish(&ctx);

  ret = tcp_appif_init(&ctx);
  if (ret != 0)
  {
    LOG_ERROR("failed to initialise appif");
    abort();
  }

  while (1)
  {
    tcp_appif_poll(&ctx);
    tcp_app_poll(&ctx);
    tcp_fast_poll(&ctx);
    tcp_cc_poll(&ctx);
    tcp_timeout_poll(&ctx);

    if (ctx.stats_log_tsc == 0)
    {
      ctx.stats_log_tsc = clock_rdtsc();
    }
    else if (clock_us_since_tsc(ctx.stats_log_tsc) >= 1000000)
    {
      LOG_INFO_PLAIN("slow_stats drops=%llu retransmissions=%llu acks_received=%llu",
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

static void tcp_state_publish(struct tcp_slow_context *ctx)
{
  int fd;
  struct tcp_state state = {
    .magic = TCP_STATE_MAGIC,
    .version = TCP_STATE_VERSION,
    .pid = getpid(),
    .ctx_addr = (__u64) ctx,
    .ctx_size = sizeof(*ctx),
    .sock_size = sizeof(struct tcp_sock),
    .port_size = sizeof(struct tcp_port),
    .flow_bucket_size = sizeof(struct tcp_flow_bucket),
    .ctl_cfg_size = sizeof(struct tcp_ctl_cfg),
    .listener_size = sizeof(struct tcp_listener_slow),
    .meta_size = sizeof(struct tcp_sock_meta_slow),
  };

  mkdir("/run/chamelio", 0777);
  fd = open(TCP_STATE_PATH, O_CREAT | O_WRONLY | O_TRUNC, 0666);
  if (fd < 0)
  {
    LOG_WARN("failed to publish TCP state");
    return;
  }

  if (write(fd, &state, sizeof(state)) != sizeof(state))
    LOG_WARN("failed to write TCP state");
  close(fd);
}

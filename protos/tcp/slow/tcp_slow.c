#include <stdlib.h>
#include <sys/epoll.h>
#include <errno.h>
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
  int busy;
  int extra_poll = 0;
  __u64 last_active_ns = 0;
  struct epoll_event wake_event = { .events = EPOLLIN };
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

  if (epoll_ctl(ctx.app_epfd, EPOLL_CTL_ADD, ctx.proto->notify_fd,
      &wake_event) != 0)
    abort();

  while (1)
  {
    busy = tcp_appif_poll(&ctx);
    busy |= tcp_app_poll(&ctx);
    busy |= tcp_fast_poll(&ctx);
    busy |= tcp_cc_poll(&ctx);
    busy |= tcp_timeout_poll(&ctx);

    if (ctx.config.slow_block && clock_canblock(&last_active_ns,
        &extra_poll, busy, (__u64) ctx.config.slow_poll_us * 1000))
    {
      __u64 deadline = ctx.stats_log_tsc + clock_us_to_tsc(1000000);
      struct to_entry *timer = tomgr_peek(ctx.tomgr);

      if (timer != NULL && timer->to < deadline)
        deadline = timer->to;
      if (ctx.n_socks != 0)
      {
        __u64 cc_deadline = ctx.cc_poll_tsc +
            clock_us_to_tsc(ctx.config.cc_control_granularity);
        if (cc_deadline < deadline)
          deadline = cc_deadline;
      }
      ret = epoll_wait(ctx.app_epfd, &wake_event, 1,
          clock_deadline_ms(deadline));
      if (ret < 0 && errno != EINTR)
        abort();
    }

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

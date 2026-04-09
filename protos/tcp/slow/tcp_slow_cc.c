#include <limits.h>

#include "clock.h"
#include "tcp_slow_internal.h"

#define TCP_SLOW_CC_BATCH 128

struct tcp_cc_stats {
  __u32 drops;
  __u32 acks;
  __u32 ackb;
  __u32 ecnb;
  __u32 rtt;
  int tx_pending;
};

static void read_stats(struct tcp_slow_context *ctx, struct tcp_sock *sock,
    struct tcp_cc_stats *stats);
static void update_sock(struct tcp_slow_context *ctx, struct tcp_sock *sock,
    struct tcp_sock_meta_slow *meta, const struct tcp_cc_stats *stats,
    __u64 elapsed_us);
static void update_const_rate(struct tcp_slow_context *ctx,
    struct tcp_sock *sock, struct tcp_sock_meta_slow *meta,
    const struct tcp_cc_stats *stats);
static void update_dctcp_rate(struct tcp_slow_context *ctx,
    struct tcp_sock *sock, struct tcp_sock_meta_slow *meta,
    const struct tcp_cc_stats *stats, __u64 elapsed_us);
static void maybe_retransmit(struct tcp_slow_context *ctx, struct tcp_sock *sock,
    struct tcp_sock_meta_slow *meta, const struct tcp_cc_stats *stats,
    __u64 now_tsc);

int tcp_slow_cc_ecn_enabled(const struct tcp_slow_context *ctx)
{
  return ctx->config.cc_algorithm == TCP_CC_ALGO_DCTCP_RATE;
}

void tcp_slow_cc_init_sock(struct tcp_slow_context *ctx, struct tcp_sock *sock)
{
  __u64 now_tsc;
  __u32 rate;
  struct tcp_sock_meta_slow *meta;

  meta = &ctx->sock_meta[sock->id];
  now_tsc = clock_rdtsc();

  sock->cc_acks = 0;
  sock->cc_ackb = 0;
  sock->cc_ecnb = 0;
  sock->cc_drops = 0;
  sock->rx_last_tsc = 0;
  sock->ack_advance_last_tsc = 0;
  sock->recovery_active = 0;
  sock->recovery_end_seq = 0;
  sock->tx_rexmit = 0;
  sock->rx_dupack_cnt = 0;

  meta->cc_tsc = now_tsc;
  meta->cc_rtt = ctx->config.cc_rtt_init;
  meta->cc_last_drops = 0;
  meta->cc_last_acks = 0;
  meta->cc_last_ackb = 0;
  meta->cc_last_ecnb = 0;
  meta->cc_rexmits = 0;
  meta->cnt_tx_pending = 0;
  meta->ts_tx_pending = 0;
  meta->dctcp_unproc_acks = 0;
  meta->dctcp_unproc_ackb = 0;
  meta->dctcp_unproc_ecnb = 0;
  meta->dctcp_unproc_drops = 0;
  meta->dctcp_ecn_rate = 0;
  meta->dctcp_act_rate = 0;
  meta->dctcp_slowstart = 0;

  switch (ctx->config.cc_algorithm)
  {
    case TCP_CC_ALGO_CONST_RATE:
      rate = ctx->config.cc_const_rate;
      break;
    case TCP_CC_ALGO_DCTCP_RATE:
      rate = ctx->config.cc_dctcp_init;
      meta->dctcp_slowstart = 1;
      break;
    default:
      rate = 0;
      break;
  }

  sock->cc_rate = rate;
}

void tcp_slow_cc_reset_sock(struct tcp_slow_context *ctx, struct tcp_sock *sock)
{
  struct tcp_sock_meta_slow *meta;

  meta = &ctx->sock_meta[sock->id];
  sock->cc_acks = 0;
  sock->cc_ackb = 0;
  sock->cc_ecnb = 0;
  sock->cc_drops = 0;
  sock->cc_rate = 0;
  sock->rx_last_tsc = 0;
  sock->ack_advance_last_tsc = 0;
  sock->recovery_active = 0;
  sock->recovery_end_seq = 0;
  sock->tx_rexmit = 0;
  sock->rx_dupack_cnt = 0;
  sock->flags &= ~(TCP_SOCK_FLAG_SHUT_RD | TCP_SOCK_FLAG_SHUT_WR |
      TCP_SOCK_FLAG_SEND_ACK | TCP_SOCK_FLAG_SEND_ECE | TCP_SOCK_FLAG_ECN);

  meta->cc_tsc = 0;
  meta->cc_rtt = 0;
  meta->cc_last_drops = 0;
  meta->cc_last_acks = 0;
  meta->cc_last_ackb = 0;
  meta->cc_last_ecnb = 0;
  meta->cc_rexmits = 0;
  meta->cnt_tx_pending = 0;
  meta->ts_tx_pending = 0;
  meta->dctcp_unproc_acks = 0;
  meta->dctcp_unproc_ackb = 0;
  meta->dctcp_unproc_ecnb = 0;
  meta->dctcp_unproc_drops = 0;
  meta->dctcp_ecn_rate = 0;
  meta->dctcp_act_rate = 0;
  meta->dctcp_slowstart = 0;
}

int tcp_slow_cc_poll(struct tcp_slow_context *ctx)
{
  int i;
  int n;
  int scanned;
  __u64 now_tsc;
  __u64 elapsed_us;
  __u32 sid;
  struct tcp_sock *sock;
  struct tcp_sock *socks;
  struct tcp_sock_meta_slow *meta;
  struct tcp_cc_stats stats;

  if (ctx->n_socks == 0)
    return 0;

  now_tsc = clock_rdtsc();
  if (ctx->cc_poll_tsc != 0 &&
      clock_us_since_tsc(ctx->cc_poll_tsc) < ctx->config.cc_control_granularity)
  {
    return 0;
  }
  ctx->cc_poll_tsc = now_tsc;

  socks = tcp_slow_get_sock_map(ctx);
  scanned = ctx->n_socks < TCP_SLOW_CC_BATCH ? ctx->n_socks : TCP_SLOW_CC_BATCH;
  n = 0;
  for (i = 0; i < scanned; i++)
  {
    if (ctx->cc_next_sock >= ctx->n_socks)
      ctx->cc_next_sock = 0;

    sid = ctx->cc_next_sock++;
    sock = &socks[sid];
    if (sock->state != TCP_SOCK_STATE_ESTABLISHED)
      continue;

    meta = &ctx->sock_meta[sid];
    if (meta->cc_tsc == 0)
    {
      meta->cc_tsc = now_tsc;
      continue;
    }

    elapsed_us = clock_us_since_tsc(meta->cc_tsc);
    if (elapsed_us < (__u64) meta->cc_rtt * ctx->config.cc_control_interval)
      continue;

    read_stats(ctx, sock, &stats);
    update_sock(ctx, sock, meta, &stats, elapsed_us);
    maybe_retransmit(ctx, sock, meta, &stats, now_tsc);

    meta->cc_tsc = now_tsc;
    n++;
  }

  return n;
}

static void read_stats(struct tcp_slow_context *ctx, struct tcp_sock *sock,
    struct tcp_cc_stats *stats)
{
  struct tcp_sock_meta_slow *meta;

  meta = &ctx->sock_meta[sock->id];
  stats->drops = sock->cc_drops - meta->cc_last_drops;
  stats->acks = sock->cc_acks - meta->cc_last_acks;
  stats->ackb = sock->cc_ackb - meta->cc_last_ackb;
  stats->ecnb = sock->cc_ecnb - meta->cc_last_ecnb;
  stats->rtt = meta->cc_rtt != 0 ? meta->cc_rtt : ctx->config.cc_rtt_init;
  stats->tx_pending = sock->tx_pending != 0;

  meta->cc_last_drops = sock->cc_drops;
  meta->cc_last_acks = sock->cc_acks;
  meta->cc_last_ackb = sock->cc_ackb;
  meta->cc_last_ecnb = sock->cc_ecnb;

  ctx->stats_drops += stats->drops;
  ctx->stats_acks_rx += stats->acks;
}

static void update_sock(struct tcp_slow_context *ctx, struct tcp_sock *sock,
    struct tcp_sock_meta_slow *meta, const struct tcp_cc_stats *stats,
    __u64 elapsed_us)
{
  switch (ctx->config.cc_algorithm)
  {
    case TCP_CC_ALGO_CONST_RATE:
      update_const_rate(ctx, sock, meta, stats);
      break;
    case TCP_CC_ALGO_DCTCP_RATE:
      update_dctcp_rate(ctx, sock, meta, stats, elapsed_us);
      break;
    default:
      sock->cc_rate = 0;
      meta->cc_rexmits = 0;
      meta->cc_rtt = ctx->config.cc_rtt_init;
      break;
  }
}

static void update_const_rate(struct tcp_slow_context *ctx,
    struct tcp_sock *sock, struct tcp_sock_meta_slow *meta,
    const struct tcp_cc_stats *stats)
{
  (void) stats;
  sock->cc_rate = ctx->config.cc_const_rate;
  meta->cc_rtt = ctx->config.cc_rtt_init;
  meta->cc_rexmits = 0;
}

static void update_dctcp_rate(struct tcp_slow_context *ctx,
    struct tcp_sock *sock, struct tcp_sock_meta_slow *meta,
    const struct tcp_cc_stats *stats, __u64 elapsed_us)
{
  __u32 act_rate;
  __u32 c_acks;
  __u32 c_ackb;
  __u32 c_drops;
  __u32 c_ecnb;
  __u32 rate;
  __u32 rtt;
  __u64 act_rate64;
  __u64 ecn_rate;

  rtt = stats->rtt != 0 ? stats->rtt : ctx->config.cc_rtt_init;
  meta->cc_rtt = rtt;

  c_ecnb = meta->dctcp_unproc_ecnb + stats->ecnb;
  c_acks = meta->dctcp_unproc_acks + stats->acks;
  c_ackb = meta->dctcp_unproc_ackb + stats->ackb;
  c_drops = meta->dctcp_unproc_drops + stats->drops;
  if (c_acks < ctx->config.cc_dctcp_minpkts)
  {
    meta->dctcp_unproc_ecnb = c_ecnb;
    meta->dctcp_unproc_acks = c_acks;
    meta->dctcp_unproc_ackb = c_ackb;
    meta->dctcp_unproc_drops = c_drops;
    return;
  }

  meta->dctcp_unproc_ecnb = 0;
  meta->dctcp_unproc_acks = 0;
  meta->dctcp_unproc_ackb = 0;
  meta->dctcp_unproc_drops = 0;

  rate = sock->cc_rate;
  if (elapsed_us != 0)
  {
    act_rate64 = ((__u64) c_ackb * 8 * 1000) / elapsed_us;
    act_rate = act_rate64 > UINT_MAX ? UINT_MAX : act_rate64;
  }
  else
    act_rate = 0;
  meta->dctcp_act_rate = (7 * meta->dctcp_act_rate + act_rate) / 8;
  if (act_rate < meta->dctcp_act_rate)
    act_rate = meta->dctcp_act_rate;

  if (rate > ((__u64) act_rate * 12) / 10)
    rate = ((__u64) act_rate * 12) / 10;

  if (meta->dctcp_slowstart)
  {
    if (c_drops == 0 && c_ecnb == 0 && meta->cc_rexmits == 0)
    {
      if ((__u64) rate * 2 <= UINT_MAX)
        rate *= 2;
      else
        rate = UINT_MAX;
    }
    else
    {
      meta->dctcp_slowstart = 0;
    }
  }

  if (!meta->dctcp_slowstart)
  {
    if (c_drops > 0 || meta->cc_rexmits > 0)
    {
      rate /= 2;
    }
    else
    {
      if (c_ackb > 0)
      {
        if (c_ecnb > c_ackb)
          c_ecnb = c_ackb;

        ecn_rate = ((__u64) c_ecnb * UINT_MAX) / c_ackb;
        ecn_rate = ecn_rate * ctx->config.cc_dctcp_weight +
            (__u64) meta->dctcp_ecn_rate *
            (UINT_MAX - ctx->config.cc_dctcp_weight);
        ecn_rate /= UINT_MAX;
        meta->dctcp_ecn_rate = ecn_rate;
      }

      if (c_ecnb > 0)
      {
        rate = ((__u64) rate * (UINT_MAX - meta->dctcp_ecn_rate / 2)) /
            UINT_MAX;
      }
      else if (ctx->config.cc_dctcp_mimd == 0)
      {
        if (UINT_MAX - rate < ctx->config.cc_dctcp_step)
          rate = UINT_MAX;
        else
          rate += ctx->config.cc_dctcp_step;
      }
      else
      {
        act_rate64 = ((__u64) rate * ctx->config.cc_dctcp_mimd) / UINT_MAX;
        if (act_rate64 > UINT_MAX - rate)
          rate = UINT_MAX;
        else
          rate += act_rate64;
      }
    }
  }

  if (rate < ctx->config.cc_dctcp_min)
    rate = ctx->config.cc_dctcp_min;

  sock->cc_rate = rate;
  meta->cc_rexmits = 0;
}

static void maybe_retransmit(struct tcp_slow_context *ctx, struct tcp_sock *sock,
    struct tcp_sock_meta_slow *meta, const struct tcp_cc_stats *stats,
    __u64 now_tsc)
{
  __u32 rtt;
  __u64 ack_adv_silence_us;
  __u64 rexmit_wait_us;
  __u64 rx_silence_us;

  rtt = stats->rtt != 0 ? stats->rtt : ctx->config.cc_rtt_init;
  rexmit_wait_us = (__u64) rtt * ctx->config.cc_control_interval *
      ctx->config.cc_rexmit_ints;
  if (rexmit_wait_us < 20000)
    rexmit_wait_us = 20000;

  rx_silence_us = sock->rx_last_tsc == 0 ? ~(__u64) 0 :
      clock_us_since_tsc(sock->rx_last_tsc);
  ack_adv_silence_us = sock->ack_advance_last_tsc == 0 ? ~(__u64) 0 :
      clock_us_since_tsc(sock->ack_advance_last_tsc);

  if (stats->tx_pending && ack_adv_silence_us >= rexmit_wait_us &&
      rx_silence_us >= rexmit_wait_us)
  {
    if (meta->cnt_tx_pending++ == 0)
    {
      meta->ts_tx_pending = now_tsc;
    }
    else if (meta->cnt_tx_pending >= ctx->config.cc_rexmit_ints &&
        clock_us_since_tsc(meta->ts_tx_pending) >= (__u64) 2 * rtt)
    {
      if (tcp_slow_state_enqueue_tx_retransmit(ctx, sock) == 0)
      {
        meta->cnt_tx_pending = 0;
        meta->cc_rexmits++;
        ctx->stats_retx++;
        return;
      }
    }
  }
  else
  {
    meta->cnt_tx_pending = 0;
  }
}

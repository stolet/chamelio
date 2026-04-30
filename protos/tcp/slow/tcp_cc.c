#include <limits.h>

#include "clock.h"
#include "log.h"
#include "tcp_internal.h"
#include "utils_sync.h"

#define TCP_CC_BATCH 128

/*** Types ********************************************************************/

struct tcp_cc_stats {
  __u32 drops;
  __u32 acks;
  __u32 ackb;
  __u32 ecnb;
  __u32 rtt;
  int tx_pending;
};

/*** CC Helpers ***************************************************************/

static void cc_stats_read(const struct tcp_slow_context *ctx,
    const struct tcp_sock *sock, const struct tcp_sock_meta_slow *meta,
    struct tcp_cc_stats *stats);
static void cc_stats_commit(struct tcp_slow_context *ctx, struct tcp_sock *sock,
    struct tcp_sock_meta_slow *meta, const struct tcp_cc_stats *stats);
static __u32 cc_rtt(const struct tcp_slow_context *ctx,
    const struct tcp_cc_stats *stats);

/*** Socket Helpers ***********************************************************/

static void cc_sock_reset(struct tcp_sock *sock, struct tcp_sock_meta_slow *meta);
static __u32 cc_init_rate(const struct tcp_slow_context *ctx,
    struct tcp_sock_meta_slow *meta);
static void sock_on_const_rate(struct tcp_slow_context *ctx,
    struct tcp_sock *sock, struct tcp_sock_meta_slow *meta,
    const struct tcp_cc_stats *stats);
static void sock_on_tx_stall(struct tcp_slow_context *ctx, struct tcp_sock *sock,
    struct tcp_sock_meta_slow *meta, const struct tcp_cc_stats *stats,
    __u64 now_tsc);
static void sock_on_cc_tick(struct tcp_slow_context *ctx, struct tcp_sock *sock,
    struct tcp_sock_meta_slow *meta, const struct tcp_cc_stats *stats,
    __u64 elapsed_us);

/*** DCTCP Helpers ************************************************************/

static __u32 dctcp_act_rate(__u32 ackb, __u64 elapsed_us);
static __u32 dctcp_ecn_rate(const struct tcp_slow_context *ctx, __u32 prev,
    __u32 ecnb, __u32 ackb);
static void sock_on_dctcp_tick(struct tcp_slow_context *ctx,
    struct tcp_sock *sock, struct tcp_sock_meta_slow *meta,
    const struct tcp_cc_stats *stats, __u64 elapsed_us);

/*** CC API *******************************************************************/

int tcp_cc_ecn_enabled(const struct tcp_slow_context *ctx)
{
  return ctx->config.cc_algorithm == TCP_CC_ALGO_DCTCP_RATE;
}

int tcp_cc_poll(struct tcp_slow_context *ctx)
{
  int i;
  int nr;
  int scanned;
  __u32 sid;
  __u64 now_tsc;
  __u64 elapsed_us;
  struct tcp_cc_stats stats;
  struct tcp_sock *sock;
  struct tcp_sock *socks;
  struct tcp_sock_meta_slow *meta;

  if (ctx->n_socks == 0)
    return 0;

  now_tsc = clock_rdtsc();
  if (ctx->cc_poll_tsc != 0 &&
      clock_us_since_tsc(ctx->cc_poll_tsc) < ctx->config.cc_control_granularity)
  {
    return 0;
  }
  ctx->cc_poll_tsc = now_tsc;

  socks = tcp_sock_map(ctx);
  scanned = ctx->n_socks < TCP_CC_BATCH ? ctx->n_socks : TCP_CC_BATCH;
  nr = 0;
  for (i = 0; i < scanned; i++)
  {
    if (ctx->cc_next_sock >= ctx->n_socks)
      ctx->cc_next_sock = 0;

    sid = ctx->cc_next_sock++;
    sock = &socks[sid];
    if (sock->state != TCP_SOCK_STATE_ESTABLISHED)
      continue;

    meta = tcp_sock_meta(ctx, sock);
    if (meta->cc_tsc == 0)
    {
      meta->cc_tsc = now_tsc;
      continue;
    }

    elapsed_us = clock_us_since_tsc(meta->cc_tsc);
    if (elapsed_us < (__u64) meta->cc_rtt * ctx->config.cc_control_interval)
      continue;

    cc_stats_read(ctx, sock, meta, &stats);
    cc_stats_commit(ctx, sock, meta, &stats);
    sock_on_cc_tick(ctx, sock, meta, &stats, elapsed_us);
    sock_on_tx_stall(ctx, sock, meta, &stats, now_tsc);
    
    meta->cc_tsc = now_tsc;
    nr++;
  }

  return nr;
}

/*** Socket API ***************************************************************/

void tcp_sock_cc_init(struct tcp_slow_context *ctx, struct tcp_sock *sock)
{
  struct tcp_sock_meta_slow *meta;

  meta = tcp_sock_meta(ctx, sock);
  cc_sock_reset(sock, meta);
  meta->cc_tsc = clock_rdtsc();
  meta->cc_rtt = sock->rtt_est != 0 ? sock->rtt_est : ctx->config.cc_rtt_init;
  sock->cc_rate = cc_init_rate(ctx, meta);
}

void tcp_sock_cc_reset(struct tcp_slow_context *ctx, struct tcp_sock *sock)
{
  cc_sock_reset(sock, tcp_sock_meta(ctx, sock));
}

/*** CC Helpers ***************************************************************/

static void cc_stats_read(const struct tcp_slow_context *ctx,
    const struct tcp_sock *sock, const struct tcp_sock_meta_slow *meta,
    struct tcp_cc_stats *stats)
{
  stats->drops = sock->cc_drops - meta->cc_last_drops;
  stats->acks = sock->cc_acks - meta->cc_last_acks;
  stats->ackb = sock->cc_ackb - meta->cc_last_ackb;
  stats->ecnb = sock->cc_ecnb - meta->cc_last_ecnb;
  if (sock->rtt_est != 0)
    stats->rtt = sock->rtt_est;
  else
    stats->rtt = meta->cc_rtt != 0 ? meta->cc_rtt : ctx->config.cc_rtt_init;
  stats->tx_pending = sock->tx_pending != 0;
}

static void cc_stats_commit(struct tcp_slow_context *ctx, struct tcp_sock *sock,
    struct tcp_sock_meta_slow *meta, const struct tcp_cc_stats *stats)
{
  meta->cc_last_drops = sock->cc_drops;
  meta->cc_last_acks = sock->cc_acks;
  meta->cc_last_ackb = sock->cc_ackb;
  meta->cc_last_ecnb = sock->cc_ecnb;

  ctx->stats_drops += stats->drops;
  ctx->stats_acks_rx += stats->acks;
}

static __u32 cc_rtt(const struct tcp_slow_context *ctx,
    const struct tcp_cc_stats *stats)
{
  return stats->rtt != 0 ? stats->rtt : ctx->config.cc_rtt_init;
}

/*** Socket Helpers ***********************************************************/

static void cc_sock_reset(struct tcp_sock *sock, struct tcp_sock_meta_slow *meta)
{
  sock->cc_acks = 0;
  sock->cc_ackb = 0;
  sock->cc_ecnb = 0;
  sock->cc_drops = 0;
  sock->cc_rate = 0;
  sock->tx_ready_tsc = 0;
  tcp_sock_recovery_reset(sock);

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

static __u32 cc_init_rate(const struct tcp_slow_context *ctx,
    struct tcp_sock_meta_slow *meta)
{
  switch (ctx->config.cc_algorithm)
  {
    case TCP_CC_ALGO_CONST_RATE:
      return ctx->config.cc_const_rate;
    case TCP_CC_ALGO_DCTCP_RATE:
      meta->dctcp_slowstart = 1;
      return ctx->config.cc_dctcp_init;
    default:
      return 0;
  }
}

static void sock_on_const_rate(struct tcp_slow_context *ctx, struct tcp_sock *sock,
    struct tcp_sock_meta_slow *meta, const struct tcp_cc_stats *stats)
{
  sock->cc_rate = ctx->config.cc_const_rate;
  meta->cc_rtt = cc_rtt(ctx, stats);
  meta->cc_rexmits = 0;
}

static void sock_on_tx_stall(struct tcp_slow_context *ctx, struct tcp_sock *sock,
    struct tcp_sock_meta_slow *meta, const struct tcp_cc_stats *stats,
    __u64 now_tsc)
{
  __u32 rtt;

  if (sock->tx_rexmit_seq != sock->tx_rexmit_end_seq)
  {
    meta->cnt_tx_pending = 0;
    return;
  }

  rtt = cc_rtt(ctx, stats);
  if (stats->tx_pending && stats->ackb == 0)
  {
    if (meta->cnt_tx_pending++ == 0)
    {
      meta->ts_tx_pending = now_tsc;
    }
    else if (meta->cnt_tx_pending >= ctx->config.cc_remit_ints &&
        clock_us_since_tsc(meta->ts_tx_pending) >= (__u64) 2 * rtt)
    {
      if (tcp_tx_retransmit(ctx, sock) == 0)
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

static void sock_on_cc_tick(struct tcp_slow_context *ctx, struct tcp_sock *sock,
    struct tcp_sock_meta_slow *meta, const struct tcp_cc_stats *stats,
    __u64 elapsed_us)
{
  switch (ctx->config.cc_algorithm)
  {
    case TCP_CC_ALGO_CONST_RATE:
      sock_on_const_rate(ctx, sock, meta, stats);
      break;
    case TCP_CC_ALGO_DCTCP_RATE:
      sock_on_dctcp_tick(ctx, sock, meta, stats, elapsed_us);
      break;
    default:
      sock->cc_rate = 0;
      meta->cc_rexmits = 0;
      meta->cc_rtt = ctx->config.cc_rtt_init;
      break;
  }
}

/*** DCTCP Helpers ************************************************************/

static __u32 dctcp_act_rate(__u32 ackb, __u64 elapsed_us)
{
  __u64 rate;

  if (elapsed_us == 0)
    return 0;

  rate = ((__u64) ackb * 8 * 1000) / elapsed_us;
  return rate > UINT_MAX ? UINT_MAX : rate;
}

static __u32 dctcp_ecn_rate(const struct tcp_slow_context *ctx, __u32 prev,
    __u32 ecnb, __u32 ackb)
{
  __u64 rate;

  rate = ((__u64) ecnb * UINT_MAX) / ackb;
  rate = rate * ctx->config.cc_dctcp_weight +
      (__u64) prev * (UINT_MAX - ctx->config.cc_dctcp_weight);
  rate /= UINT_MAX;
  return rate;
}

static void sock_on_dctcp_tick(struct tcp_slow_context *ctx,
    struct tcp_sock *sock, struct tcp_sock_meta_slow *meta,
    const struct tcp_cc_stats *stats, __u64 elapsed_us)
{
  __u32 acks;
  __u32 ackb;
  __u32 act_rate;
  __u32 drops;
  __u32 ecnb;
  __u32 rate;
  __u64 step;

  meta->cc_rtt = cc_rtt(ctx, stats);

  ecnb = meta->dctcp_unproc_ecnb + stats->ecnb;
  acks = meta->dctcp_unproc_acks + stats->acks;
  ackb = meta->dctcp_unproc_ackb + stats->ackb;
  drops = meta->dctcp_unproc_drops + stats->drops;
  if (acks < ctx->config.cc_dctcp_minpkts)
  {
    meta->dctcp_unproc_ecnb = ecnb;
    meta->dctcp_unproc_acks = acks;
    meta->dctcp_unproc_ackb = ackb;
    meta->dctcp_unproc_drops = drops;
    return;
  }

  meta->dctcp_unproc_ecnb = 0;
  meta->dctcp_unproc_acks = 0;
  meta->dctcp_unproc_ackb = 0;
  meta->dctcp_unproc_drops = 0;

  rate = sock->cc_rate;
  act_rate = dctcp_act_rate(ackb, elapsed_us);
  meta->dctcp_act_rate = (7 * meta->dctcp_act_rate + act_rate) / 8;

  if (meta->dctcp_slowstart)
  {
    if (drops == 0 && ecnb == 0 && meta->cc_rexmits == 0)
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
    if (drops > 0 || meta->cc_rexmits > 0)
    {
      rate /= 2;
    }
    else
    {
      if (ackb > 0)
      {
        if (ecnb > ackb)
          ecnb = ackb;
        meta->dctcp_ecn_rate = dctcp_ecn_rate(ctx, meta->dctcp_ecn_rate, ecnb,
            ackb);
      }

      if (ecnb > 0)
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
        step = ((__u64) rate * ctx->config.cc_dctcp_mimd) / UINT_MAX;
        if (step > UINT_MAX - rate)
          rate = UINT_MAX;
        else
          rate += step;
      }
    }
  }

  if (rate < ctx->config.cc_dctcp_min)
    rate = ctx->config.cc_dctcp_min;

  sock->cc_rate = rate;
  meta->cc_rexmits = 0;
}

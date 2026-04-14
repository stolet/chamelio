#include <rte_mbuf.h>

#include "fast.h"
#include "txcache.h"

int txcache_alloc(struct fast_context *ctx, struct rte_mbuf ***mbs, __u16 num)
{
  __u16 grow, tail, g, added;

  if (num == 0)
    return 0;

  if (ctx->tx_cache_n < num)
  {
    grow = TX_CACHE_SIZE - ctx->tx_cache_n;
    tail = (ctx->tx_cache_head + ctx->tx_cache_n) & (TX_CACHE_SIZE - 1);
    added = 0;

    if (tail + grow <= TX_CACHE_SIZE)
    {
      if (rte_pktmbuf_alloc_bulk(ctx->nic_ctx.pool,
            ctx->tx_cache_mbs + tail, grow) == 0)
      {
        added = grow;
      }
    }
    else
    {
      g = TX_CACHE_SIZE - tail;
      if (rte_pktmbuf_alloc_bulk(ctx->nic_ctx.pool,
            ctx->tx_cache_mbs + tail, g) == 0)
      {
        added = g;
        if (rte_pktmbuf_alloc_bulk(ctx->nic_ctx.pool,
              ctx->tx_cache_mbs, grow - g) == 0)
        {
          added += (grow - g);
        }
      }
    }

    ctx->tx_cache_n += added;
  }

  num = MIN(num, (ctx->tx_cache_head + ctx->tx_cache_n <= TX_CACHE_SIZE ?
        ctx->tx_cache_n : TX_CACHE_SIZE - ctx->tx_cache_head));
  *mbs = ctx->tx_cache_mbs + ctx->tx_cache_head;

  ctx->tx_cache_head = (ctx->tx_cache_head + num) & (TX_CACHE_SIZE - 1);
  ctx->tx_cache_n -= num;
  return num;
}

void txcache_free(struct fast_context *ctx, struct rte_mbuf *mb)
{
  __u16 n, head;

  n = ctx->tx_cache_n;
  if (n < TX_CACHE_SIZE)
  {
    if (ctx->tx_cache_head == 0)
      head = TX_CACHE_SIZE - 1;
    else
      head = (ctx->tx_cache_head - 1) & (TX_CACHE_SIZE - 1);

    ctx->tx_cache_mbs[head] = mb;
    ctx->tx_cache_head = head;
    ctx->tx_cache_n = n + 1;
    mb->ol_flags = 0;
  }
  else
  {
    mb->ol_flags = 0;
    rte_pktmbuf_free(mb);
  }
}

void txcache_unalloc(struct fast_context *ctx, __u16 num)
{
  if (num == 0)
    return;

  ctx->tx_cache_head = (ctx->tx_cache_head - num) & (TX_CACHE_SIZE - 1);
  ctx->tx_cache_n += num;
}

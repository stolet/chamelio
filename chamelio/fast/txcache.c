#include <linux/types.h>

#include "fast.h"
#include "utils.h"

int txcache_alloc(struct fast_context *ctx, 
    struct rte_mbuf ***mbs, __u16 num)
{
  __u16 grow, tail, g;

  /* We don't have enough mbufs in the cache so allocate more */
  if (ctx->tx_cache_n < num)
  {
    grow = TX_CACHE_SIZE - ctx->tx_cache_n;
    tail = (ctx->tx_cache_head + ctx->tx_cache_n) & (TX_CACHE_SIZE - 1);

    if (tail + grow <= TX_CACHE_SIZE)
    {
      assert(rte_pktmbuf_alloc_bulk(ctx->nic_ctx.pool, 
          ctx->tx_cache_mbs + tail , grow) == 0);
    }
    else
    {
      g = TX_CACHE_SIZE - tail;
      assert(rte_pktmbuf_alloc_bulk(ctx->nic_ctx.pool, 
          ctx->tx_cache_mbs + tail, g) == 0);
      assert(rte_pktmbuf_alloc_bulk(ctx->nic_ctx.pool, 
          ctx->tx_cache_mbs, grow - g) == 0);
    }

    ctx->tx_cache_n += grow;
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
    /* Return mbuf to the cache */
    head = (ctx->tx_cache_head + n) & (TX_CACHE_SIZE - 1);
    ctx->tx_cache_mbs[head] = mb;
    ctx->tx_cache_n = n + 1;
    mb->ol_flags = 0;
  }
  else
  {
    /* The cache is full so return to the DPDK mempool */
    mb->ol_flags = 0;
    rte_pktmbuf_free(mb);
  }
}
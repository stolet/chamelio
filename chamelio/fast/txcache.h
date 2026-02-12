#ifndef TXCACHE_H_
#define TXCACHE_H_

#include <linux/types.h>

/* Allocates mbufs from the cache or from dpdk if the cache is empty */
static inline int txcache_alloc(struct fast_context *ctx, 
    struct rte_mbuf ***mbs, __u16 num)
{
  __u16 grow, tail, g, added;
  
  if (num == 0)
    return 0;

  /* We don't have enough mbufs in the cache so allocate more */
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

/* Returns mbuf to the cache but if cache is full free to DPDK */
static inline void txcache_free(struct fast_context *ctx, 
    struct rte_mbuf *mb)
{
  __u16 n, head;

  n = ctx->tx_cache_n;
  if (n < TX_CACHE_SIZE)
  {
    /* Return mbuf to the cache */
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
    /* The cache is full so return to the DPDK mempool */
    mb->ol_flags = 0;
    rte_pktmbuf_free(mb);
  }
}

/* Unallocate top num buffers to return them to the cache */
static inline void txcache_unalloc(struct fast_context *ctx, __u16 num)
{
  if (num == 0)
    return;

  ctx->tx_cache_head = (ctx->tx_cache_head - num) & (TX_CACHE_SIZE - 1);
  ctx->tx_cache_n += num;
}

#endif

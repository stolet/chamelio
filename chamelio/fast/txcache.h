#ifndef TXCACHE_H_
#define TXCACHE_H_

#include <linux/types.h>

struct fast_context;
struct rte_mbuf;

int txcache_alloc(struct fast_context *ctx, struct rte_mbuf ***mbs, __u16 num);
void txcache_free(struct fast_context *ctx, struct rte_mbuf *mb);
void txcache_unalloc(struct fast_context *ctx, __u16 num);

#endif

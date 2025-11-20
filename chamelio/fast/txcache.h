#ifndef TXCACHE_H_
#define TXCACHE_H_

#include <linux/types.h>

int txcache_alloc(struct fast_context *ctx, 
    struct rte_mbuf ***mbs, __u16 num);
void txcache_free(struct fast_context *ctx, struct rte_mbuf *mb);

#endif
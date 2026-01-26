#include <stdlib.h>
#include <linux/types.h>

#include <rte_mbuf.h>

#include "fast.h"
#include "fast_comb.h"
#include "fast_ebpf.h"
#include "udp.h"
#include "clock.h"
#include "infra.h"
#include "ebpf.h"

static inline void rx_poll_guest(struct guest_fast *g,
    struct rte_mbuf *mb, __u64 pkt_off, __u64 tsc_start);
static inline void rx_poll_guest_comb(struct guest_fast *g,
    struct rte_mbuf *mb, __u64 pkt_off, __u64 tsc_start);

int fast_rx_poll(struct fast_context *ctx)
{
  int i, n;
  struct rte_mbuf *mbs[FAST_BATCH_SIZE];
  struct guest_fast *g;
  __u64 tsc_start, pkt_off;
  const int use_comb = ctx->config->fp_jit_combined;

  n = FAST_BATCH_SIZE;
  if (TXBUF_SIZE - ctx->tx_n < n)
    n = TXBUF_SIZE - ctx->tx_n;

  /* Receive packets from the NIC */
  n = nic_fast_rx(&ctx->nic_ctx, n, mbs);

  /* Return if we received no packets */
  if (n <= 0)
    return 0;

  /* Prefetch first two mbuf cachelines */
  utils_prefetch0(rte_pktmbuf_mtod(mbs[0], __u8 *));
  utils_prefetch0(rte_pktmbuf_mtod(mbs[0] + 64, __u8 *));

  for (i = 0; i < n; i++)
  {
    /* Prefetch next mbuf two cachelines */
    if (i + 1 < n)
    {
      utils_prefetch0(rte_pktmbuf_mtod(mbs[i + 1], __u8 *));
      utils_prefetch0(rte_pktmbuf_mtod(mbs[i + 1] + 64, __u8 *));
    }

    /* Process infrastructure protocols */
    tsc_start = clock_rdtsc();
    g = infra_rx(ctx, mbs[i], &pkt_off);
    if (g == NULL)
      continue;

    /* Drop if this guest is out of budget */
    if (__atomic_load_n(g->budget, __ATOMIC_RELAXED) <= 0)
      continue;

    if (use_comb)
      rx_poll_guest_comb(g, mbs[i], pkt_off, tsc_start);
    else
      rx_poll_guest(g, mbs[i], pkt_off, tsc_start);
  }

  /* Return used mbufs to the mbuf pool */
  rte_pktmbuf_free_bulk(mbs, n);

  return n;
}

static inline void rx_poll_guest(struct guest_fast *g,
    struct rte_mbuf *mb, __u64 pkt_off, __u64 tsc_start)
{
  int ret;
  __u64 tsc_spent;

  fast_ebpf_ctx_set_pkt(&g->proto.ebpf_ctx, mb, pkt_off);
  ebpf_vm_exec(g->proto.event_rx_vm, &g->proto.ebpf_ctx,
      sizeof(struct cham_ebpf_ctx), &ret);

  tsc_spent = clock_rdtsc() - tsc_start;
  __atomic_fetch_sub(g->budget, tsc_spent, __ATOMIC_RELAXED);
}

static inline void rx_poll_guest_comb(struct guest_fast *g,
    struct rte_mbuf *mb, __u64 pkt_off, __u64 tsc_start)
{
  int ret;
  __u64 tsc_spent;
  struct fast_comb_rx_ctx jit_ctx = {
    .g = g,
    .mb = mb,
    .pkt_off = pkt_off,
  };

  ebpf_vm_exec(g->proto.event_rx_vm, &jit_ctx, sizeof(jit_ctx), &ret);

  if (ret > 0)
  {
    tsc_spent = clock_rdtsc() - tsc_start;
    __atomic_fetch_sub(g->budget, tsc_spent, __ATOMIC_RELAXED);
  }
}

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
#include "txcache.h"

static inline void rx_poll_guest(struct guest_fast *g,
    struct rte_mbuf *mb, __u64 pkt_off, __u64 tsc_start, int charge_budget,
    int virt_gre);
static inline void rx_poll_guest_comb(struct guest_fast *g,
    struct rte_mbuf *mb, __u64 pkt_off, __u64 tsc_start, int charge_budget,
    int virt_gre);

int fast_rx_poll(struct fast_context *ctx)
{
  int i, nrx;
  struct rte_mbuf *mbs[FAST_BATCH_SIZE];
  struct guest_fast *g;
  __u64 tsc_start = 0, pkt_off;
  const int use_comb = ctx->fp_jit_combined;
  const int charge_budget = ctx->perf_iso;

  nrx = FAST_BATCH_SIZE;
  if (TXBUF_SIZE - ctx->tx_n < nrx)
    nrx = TXBUF_SIZE - ctx->tx_n;

  /* Receive packets from the NIC */
  nrx = nic_fast_rx(&ctx->nic_ctx, nrx, mbs);

  /* Return if we received no packets */
  if (nrx <= 0)
    return 0;

  /* Prefetch first two mbuf cachelines */
  rte_prefetch0(rte_pktmbuf_mtod(mbs[0], __u8 *));
  rte_prefetch0(rte_pktmbuf_mtod(mbs[0], __u8 *) + 64);

  for (i = 0; i < nrx; i++)
  {
    /* Prefetch next mbuf two cachelines */
    if (i + 1 < nrx)
    {
      rte_prefetch0(rte_pktmbuf_mtod(mbs[i + 1], __u8 *));
      rte_prefetch0(rte_pktmbuf_mtod(mbs[i + 1], __u8 *) + 64);
    }

    /* Process infrastructure protocols */
    if (charge_budget)
      tsc_start = clock_rdtsc();

    g = infra_rx(ctx, mbs[i], &pkt_off);
    if (g == NULL)
      continue;

    /* Drop if this guest is out of budget */
    if (charge_budget && __atomic_load_n(g->budget, __ATOMIC_RELAXED) <= 0)
      continue;

    if (use_comb)
      rx_poll_guest_comb(g, mbs[i], pkt_off, tsc_start, charge_budget,
          ctx->virt_gre);
    else
      rx_poll_guest(g, mbs[i], pkt_off, tsc_start, charge_budget,
          ctx->virt_gre);
  }

  /* Reuse mbufs in txcache */
  for (i = 0; i < nrx; i++)
    txcache_free(ctx, mbs[i]);

  return nrx;
}

static inline void rx_poll_guest(struct guest_fast *g,
    struct rte_mbuf *mb, __u64 pkt_off, __u64 tsc_start, int charge_budget,
    int virt_gre)
{
  int ret;
  __u64 tsc_spent;

  fast_ebpf_ctx_set_pkt(&g->proto.ebpf_ctx, mb, pkt_off, virt_gre);
  ebpf_vm_exec(g->proto.event_rx_vm, &g->proto.ebpf_ctx,
      sizeof(struct cham_ebpf_ctx), &ret);

  if (charge_budget)
  {
    tsc_spent = clock_rdtsc() - tsc_start;
    __atomic_fetch_sub(g->budget, tsc_spent, __ATOMIC_RELAXED);
  }
}

static inline void rx_poll_guest_comb(struct guest_fast *g,
    struct rte_mbuf *mb, __u64 pkt_off, __u64 tsc_start, int charge_budget,
    int virt_gre)
{
  int ret;
  __u64 tsc_spent;
  struct fast_comb_rx_ctx jit_ctx = {
    .g = g,
    .mb = mb,
    .pkt_off = pkt_off,
    .virt_gre = virt_gre,
  };

  ebpf_vm_exec(g->proto.event_rx_vm, &jit_ctx, sizeof(jit_ctx), &ret);

  if (charge_budget && ret > 0)
  {
    tsc_spent = clock_rdtsc() - tsc_start;
    __atomic_fetch_sub(g->budget, tsc_spent, __ATOMIC_RELAXED);
  }
}

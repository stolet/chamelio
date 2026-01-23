#include <stdlib.h>
#include <linux/types.h>

#include <rte_mbuf.h>

#include "fast.h"
#include "fast_jit.h"
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

  n = FAST_BATCH_SIZE;
  if (TXBUF_SIZE - ctx->tx_n < n)
    n = TXBUF_SIZE - ctx->tx_n;

  /* Receive packets from the NIC */
  n = nic_fast_rx(&ctx->nic_ctx, n, mbs);

  /* Return if we received no packets */
  if (n <= 0)
    return 0;

  for (i = 0; i < n; i++)
  {
    /* Process infrastructure protocols */
    tsc_start = clock_rdtsc();
    g = infra_rx(ctx, mbs[i], &pkt_off);
    
    /* Execute custom protocol rx procedure */
    if (g != NULL)
    {
      /* Drop if this guest is out of budget */
      if (__atomic_load_n(g->budget, __ATOMIC_RELAXED) <= 0)
        continue;

      if (ctx->config->fp_jit_combined)
        rx_poll_guest_comb(g, mbs[i], pkt_off, tsc_start);
      else
        rx_poll_guest(g, mbs[i], pkt_off, tsc_start);
    }
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

  g->proto.ebpf_ctx.pkt = rte_pktmbuf_mtod(mb, __u8 *) + pkt_off;
  g->proto.ebpf_ctx.pkt_end = (void *) (g->proto.ebpf_ctx.pkt + UDP_MSS);
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
  struct fast_jit_rx_ctx jit_ctx = {
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
#include <stdlib.h>
#include <linux/types.h>

#include <rte_mbuf.h>

#include "fast.h"
#include "fast_ebpf.h"
#include "udp.h"
#include "clock.h"
#include "infra.h"
#include "ebpf.h"
#include "txcache.h"
#include "ip_hdr.h"
#include "protos.h"

static inline int rx_poll_guest(struct fast_context *ctx, struct guest_fast *g,
    struct rte_mbuf *mb, __u64 pkt_off, __u64 tsc_start, int charge_budget,
    int virt_gre);

int fast_rx_poll(struct fast_context *ctx)
{
  int i, nrx;
  struct rte_mbuf *mbs[FAST_RX_BATCH_SIZE];
  __u8 tx_idx[FAST_RX_BATCH_SIZE] = {0};
  struct guest_fast *g;
  __u64 tsc_start = 0, pkt_off;
  const int charge_budget = ctx->perf_iso;

  nrx = FAST_RX_BATCH_SIZE;
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
    // if (charge_budget && __atomic_load_n(g->budget, __ATOMIC_RELAXED) <= 0)
    //   continue;

    tx_idx[i] = (__u8) rx_poll_guest(ctx, g, mbs[i], pkt_off, tsc_start,
        charge_budget, ctx->virt_gre);
  }

  fast_txflush(ctx);

  /* Reuse mbufs in txcache */
  for (i = 0; i < nrx; i++)
  {
    if (!tx_idx[i])
      txcache_free(ctx, mbs[i]);
  }

  return nrx;
}

static inline int rx_poll_guest(struct fast_context *ctx, struct guest_fast *g,
    struct rte_mbuf *mb, __u64 pkt_off, __u64 tsc_start, int charge_budget,
    int virt_gre)
{
  int exec_ret;
  int ret, rx_ret;
  int mb_tx = 0;
  int did_work;
  __u64 tsc_spent;

  did_work = 0;
  fast_ebpf_ctx_set_pkt(&g->proto.ebpf_ctx, mb, pkt_off, virt_gre);
  switch (ctx->fp_proto_mode)
  {
    case FP_PROTO_EBPF:
      exec_ret = ebpf_vm_exec(g->proto.event_rx_vm, &g->proto.ebpf_ctx,
          sizeof(struct cham_ebpf_ctx), &rx_ret);
      if (exec_ret >= 0 && rx_ret >= 0)
        did_work = 1;
      break;
    case FP_PROTO_HAND:
      rx_ret = proto_hand_event_rx(g->proto.proto_type, &g->proto.ebpf_ctx);
      if (rx_ret >= 0)
        did_work = 1;
      break;
    default:
      exec_ret = -1;
      rx_ret = -1;
      break;
  }

  if (rx_ret > 0)
  {
    ret = infra_tx(ctx, g, mb, rx_ret);
    if (ret == 0)
    {
      ctx->tx_mbs[ctx->tx_n] = mb;
      ctx->tx_n++;
      mb_tx = 1;
    }
  }

  if (charge_budget && did_work)
  {
    tsc_spent = clock_rdtsc() - tsc_start;
    __atomic_fetch_sub(g->budget, tsc_spent, __ATOMIC_RELAXED);
    fast_budg_add(ctx, g->id, tsc_spent);
  }

  return mb_tx;
}

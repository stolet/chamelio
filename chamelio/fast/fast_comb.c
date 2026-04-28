#include <stddef.h>

#include <rte_mbuf.h>

#include "fast.h"
#include "fast_comb.h"
#include "fast_ebpf.h"
#include "infra.h"
#include "txcache.h"
#include "clock.h"
#include "queue_fns.h"
#include "scheduler_fns.h"

static inline int sched_poll_slot(struct fast_context *ctx, int slot,
    struct rte_mbuf **mbs, int max, int *ntx, int charge_budget);
static inline int rx_poll_slot(struct fast_context *ctx,
    struct guest_fast *g, struct rte_mbuf *mb, __u64 pkt_off,
    __u64 tsc_start, int charge_budget);
static inline int deq_poll_slot(struct fast_context *ctx, int slot,
    struct rte_mbuf **mbs, int max, int *ntx, int *ndeq, int charge_budget);
static inline void dqueue_rotate_head(struct guest_fast *g,
    struct cham_dqueue *qcur);

#ifdef CHAM_COMB_VIRT_GRE
#define comb_virt_gre(_ctx) CHAM_COMB_VIRT_GRE
#else
#define comb_virt_gre(_ctx) ((_ctx)->virt_gre)
#endif

#ifdef CHAM_COMB_PERF_ISO
#define comb_perf_iso(_ctx) CHAM_COMB_PERF_ISO
#else
#define comb_perf_iso(_ctx) ((_ctx)->perf_iso)
#endif

#define DEFINE_EVENT_STUB(kind, slot)                                         \
  __attribute__((weak)) uint64_t event_##kind##_slot_##slot(void *mem,        \
      size_t mem_len)                                                         \
  {                                                                           \
    (void) mem;                                                               \
    (void) mem_len;                                                           \
    return 0;                                                                 \
  }

DEFINE_EVENT_STUB(rx, 0)
DEFINE_EVENT_STUB(rx, 1)
DEFINE_EVENT_STUB(rx, 2)
DEFINE_EVENT_STUB(rx, 3)
DEFINE_EVENT_STUB(deq, 0)
DEFINE_EVENT_STUB(deq, 1)
DEFINE_EVENT_STUB(deq, 2)
DEFINE_EVENT_STUB(deq, 3)
DEFINE_EVENT_STUB(sched, 0)
DEFINE_EVENT_STUB(sched, 1)
DEFINE_EVENT_STUB(sched, 2)
DEFINE_EVENT_STUB(sched, 3)

#define DEFINE_RX_SLOT(slot)                                                  \
  static inline int rx_poll_slot_##slot(struct fast_context *ctx,             \
      struct guest_fast *g, struct rte_mbuf *mb, __u64 pkt_off,               \
      __u64 tsc_start, int charge_budget)                                     \
  {                                                                           \
    int did_work = 0;                                                         \
    int ret;                                                                  \
    int rx_ret;                                                               \
    int mb_tx = 0;                                                            \
    __u64 tsc_spent;                                                          \
                                                                              \
    if (!g->proto.has_event_rx)                                               \
      return 0;                                                               \
                                                                              \
    fast_ebpf_ctx_set_pkt(&g->proto.ebpf_ctx, mb, pkt_off,                    \
        comb_virt_gre(ctx));                                                  \
    rx_ret = (int) event_rx_slot_##slot(&g->proto.ebpf_ctx,                   \
        sizeof(struct cham_ebpf_ctx));                                        \
    if (rx_ret >= 0)                                                          \
      did_work = 1;                                                           \
    if (rx_ret > 0)                                                           \
    {                                                                         \
      ret = infra_tx(ctx, g, mb, rx_ret);                                     \
      if (ret == 0)                                                           \
      {                                                                       \
        ctx->tx_mbs[ctx->tx_n] = mb;                                          \
        ctx->tx_n++;                                                          \
        mb_tx = 1;                                                            \
      }                                                                       \
    }                                                                         \
                                                                              \
    if (charge_budget && did_work)                                            \
    {                                                                         \
      tsc_spent = clock_rdtsc() - tsc_start;                                  \
      __atomic_fetch_sub(g->budget, tsc_spent, __ATOMIC_RELAXED);             \
      fast_budg_add(ctx, g->id, tsc_spent);                                   \
    }                                                                         \
                                                                              \
    return mb_tx;                                                             \
  }

#define DEFINE_SCHED_SLOT(slot)                                               \
  static inline int sched_poll_slot_##slot(struct fast_context *ctx,          \
      struct rte_mbuf **mbs, int max, int *ntx, int charge_budget)            \
  {                                                                           \
    int did_work;                                                             \
    int ntx_start;                                                            \
    int sched_ret;                                                            \
    int ret;                                                                  \
    __u64 tsc_start = 0, tsc_spent;                                           \
    struct guest_fast *g = &ctx->guests[slot];                                \
                                                                              \
    if (!g->proto.has_event_sched)                                            \
      return 0;                                                               \
                                                                              \
    if (sched_head(&g->proto.ebpf_ctx.sched) == NULL)                         \
      return 0;                                                               \
                                                                              \
    if (charge_budget && __atomic_load_n(g->budget, __ATOMIC_RELAXED) <= 0)   \
      return 0;                                                               \
                                                                              \
    if (charge_budget)                                                        \
      tsc_start = clock_rdtsc();                                              \
    ntx_start = *ntx;                                                         \
    did_work = 0;                                                             \
    sched_ret = 0;                                                            \
    while (*ntx < max)                                                        \
    {                                                                         \
      struct rte_mbuf *mb;                                                    \
                                                                              \
      if (sched_head(&g->proto.ebpf_ctx.sched) == NULL)                       \
        break;                                                                \
                                                                              \
      mb = mbs[*ntx];                                                         \
      mb->data_off = 0;                                                       \
      fast_ebpf_ctx_set_pkt_l2(&g->proto.ebpf_ctx, mb,                        \
          comb_virt_gre(ctx));                                                \
                                                                              \
      sched_ret = (int) event_sched_slot_##slot(&g->proto.ebpf_ctx,           \
          sizeof(struct cham_ebpf_ctx));                                      \
      if (sched_ret <= 0)                                                     \
        break;                                                                \
      ret = infra_tx(ctx, g, mb, sched_ret);                                  \
      if (ret == 0)                                                           \
      {                                                                       \
        ctx->tx_mbs[ctx->tx_n] = mb;                                          \
        ctx->tx_n++;                                                          \
        (*ntx)++;                                                             \
        did_work = 1;                                                         \
      }                                                                       \
    }                                                                         \
                                                                              \
    if (charge_budget && *ntx > ntx_start)                                    \
    {                                                                         \
      tsc_spent = clock_rdtsc() - tsc_start;                                  \
      __atomic_fetch_sub(g->budget, tsc_spent, __ATOMIC_RELAXED);             \
      fast_budg_add(ctx, g->id, tsc_spent);                                   \
    }                                                                         \
                                                                              \
    if (sched_ret < 0)                                                        \
      return -1;                                                              \
                                                                              \
    return did_work;                                                          \
  }

#define DEFINE_DEQ_SLOT(slot)                                                 \
  static inline int deq_poll_slot_##slot(struct fast_context *ctx,            \
      struct rte_mbuf **mbs, int max, int *ntx, int *ndeq,                    \
      int charge_budget)                                                      \
  {                                                                           \
    int deq_ret;                                                              \
    int did_work;                                                             \
    int ndeq_start;                                                           \
    int ret;                                                                  \
    int j;                                                                    \
    struct guest_fast *g = &ctx->guests[slot];                                \
    struct cham_dqueue *qcur;                                                 \
    struct queue_entry *qe;                                                   \
    __u64 tsc_start = 0, tsc_spent;                                           \
                                                                              \
    if (!g->proto.has_event_deq)                                              \
      return 0;                                                               \
                                                                              \
    if (g->proto.dqueues_head == PROTOQ_ID_INVALID)                           \
      return 0;                                                               \
                                                                              \
    if (charge_budget && __atomic_load_n(g->budget, __ATOMIC_RELAXED) <= 0)   \
      return 0;                                                               \
                                                                              \
    if (charge_budget)                                                        \
      tsc_start = clock_rdtsc();                                              \
    did_work = 0;                                                             \
    ndeq_start = *ndeq;                                                       \
    for (j = 0; j < g->proto.ndqueues && *ndeq < max; j++)                    \
    {                                                                         \
      qcur = &g->proto.dqueues[g->proto.dqueues_head];                        \
      while (*ndeq < max)                                                     \
      {                                                                       \
        struct rte_mbuf *mb;                                                  \
                                                                              \
        qe = queue_head(&qcur->dq);                                           \
        if (qe == NULL)                                                       \
          break;                                                              \
                                                                              \
        mb = mbs[*ntx];                                                       \
        mb->data_off = 0;                                                     \
        fast_ebpf_ctx_set_pkt_l2(&g->proto.ebpf_ctx, mb,                      \
            comb_virt_gre(ctx));                                              \
                                                                              \
        g->proto.ebpf_ctx.qe = qe;                                            \
        g->proto.ebpf_ctx.qid = qcur->id;                                     \
                                                                              \
        deq_ret = (int) event_deq_slot_##slot(&g->proto.ebpf_ctx,             \
            sizeof(struct cham_ebpf_ctx));                                    \
        (*ndeq)++;                                                            \
        did_work = 1;                                                         \
                                                                              \
        if (deq_ret < 0)                                                      \
          return -1;                                                          \
                                                                              \
        if (deq_ret > 0)                                                      \
        {                                                                     \
          ret = infra_tx(ctx, g, mb, deq_ret);                                \
          if (ret == 0)                                                       \
          {                                                                   \
            ctx->tx_mbs[ctx->tx_n] = mb;                                      \
            ctx->tx_n++;                                                      \
            (*ntx)++;                                                         \
          }                                                                   \
        }                                                                     \
                                                                              \
        ret = queue_dequeue(&qcur->dq);                                       \
        if (ret != 0)                                                         \
          return -1;                                                          \
      }                                                                       \
                                                                              \
      dqueue_rotate_head(g, qcur);                                            \
    }                                                                         \
                                                                              \
    if (charge_budget && *ndeq > ndeq_start)                                  \
    {                                                                         \
      tsc_spent = clock_rdtsc() - tsc_start;                                  \
      __atomic_fetch_sub(g->budget, tsc_spent, __ATOMIC_RELAXED);             \
      fast_budg_add(ctx, g->id, tsc_spent);                                   \
    }                                                                         \
                                                                              \
    return did_work;                                                          \
  }

DEFINE_RX_SLOT(0)
DEFINE_RX_SLOT(1)
DEFINE_RX_SLOT(2)
DEFINE_RX_SLOT(3)
DEFINE_SCHED_SLOT(0)
DEFINE_SCHED_SLOT(1)
DEFINE_SCHED_SLOT(2)
DEFINE_SCHED_SLOT(3)
DEFINE_DEQ_SLOT(0)
DEFINE_DEQ_SLOT(1)
DEFINE_DEQ_SLOT(2)
DEFINE_DEQ_SLOT(3)

uint64_t fast_rx_poll_comb(void *mem, size_t mem_len)
{
  int i;
  int nrx;
  struct fast_context *ctx = mem;
  struct guest_fast *g;
  struct rte_mbuf *mbs[FAST_RX_BATCH_SIZE];
  __u8 tx_idx[FAST_RX_BATCH_SIZE] = {0};
  __u64 tsc_start = 0, pkt_off;
  const int charge_budget = comb_perf_iso(ctx);

  (void) mem_len;
  nrx = FAST_RX_BATCH_SIZE;
  if (TXBUF_SIZE - ctx->tx_n < nrx)
    nrx = TXBUF_SIZE - ctx->tx_n;

  nrx = nic_fast_rx(&ctx->nic_ctx, nrx, mbs);
  if (nrx <= 0)
    return 0;

  rte_prefetch0(rte_pktmbuf_mtod(mbs[0], __u8 *));
  rte_prefetch0(rte_pktmbuf_mtod(mbs[0], __u8 *) + 64);

  for (i = 0; i < nrx; i++)
  {
    if (i + 1 < nrx)
    {
      rte_prefetch0(rte_pktmbuf_mtod(mbs[i + 1], __u8 *));
      rte_prefetch0(rte_pktmbuf_mtod(mbs[i + 1], __u8 *) + 64);
    }

    if (charge_budget)
      tsc_start = clock_rdtsc();

    g = infra_rx(ctx, mbs[i], &pkt_off);
    if (g == NULL)
      continue;

    // if (charge_budget && __atomic_load_n(g->budget, __ATOMIC_RELAXED) <= 0)
    //   continue;

    tx_idx[i] = (__u8) rx_poll_slot(ctx, g, mbs[i], pkt_off, tsc_start,
        charge_budget);
  }

  fast_txflush(ctx);

  for (i = 0; i < nrx; i++)
  {
    if (!tx_idx[i])
      txcache_free(ctx, mbs[i]);
  }

  return nrx;
}

uint64_t fast_queues_poll_comb(void *mem, size_t mem_len)
{
  int i, gid, max, ret, ndeq, ntx, last_queues_guest;
  struct fast_context *ctx = mem;
  struct rte_mbuf **mbs;
  __u8 start_guest = ctx->next_queues_guest;
  const int charge_budget = comb_perf_iso(ctx);

  (void) mem_len;
  if (ctx->n_guests == 0)
    return 0;

  if (start_guest >= ctx->n_guests)
    start_guest = 0;

  max = FAST_DEQ_BATCH_SIZE;
  if (TXBUF_SIZE - ctx->tx_n < max)
    max = TXBUF_SIZE - ctx->tx_n;

  max = txcache_alloc(ctx, &mbs, max);
  if (max <= 0)
    return 0;

  rte_prefetch0(rte_pktmbuf_mtod(mbs[0], __u8 *));
  rte_prefetch0(rte_pktmbuf_mtod(mbs[0], __u8 *) + 64);

  ntx = 0;
  ndeq = 0;
  last_queues_guest = -1;
  for (i = 0; i < ctx->n_guests && ndeq < max; i++)
  {
    if (ndeq + 1 < max)
    {
      rte_prefetch0(rte_pktmbuf_mtod(mbs[ndeq + 1], __u8 *));
      rte_prefetch0(rte_pktmbuf_mtod(mbs[ndeq + 1], __u8 *) + 64);
    }

    gid = (start_guest + i) % ctx->n_guests;
    ret = deq_poll_slot(ctx, gid, mbs, max, &ntx, &ndeq, charge_budget);
    if (ret < 0)
      return (uint64_t) -1;
    if (ret > 0)
      last_queues_guest = gid;
  }

  if (last_queues_guest >= 0)
    ctx->next_queues_guest = (last_queues_guest + 1) % ctx->n_guests;

  txcache_unalloc(ctx, max - ntx);
  return ndeq;
}

uint64_t fast_sched_poll_comb(void *mem, size_t mem_len)
{
  int max, ret;
  int i, gid, ntx, has_sched_work, last_sched_guest;
  struct fast_context *ctx = mem;
  struct guest_fast *g;
  struct rte_mbuf **mbs;
  __u8 n_guests = ctx->n_guests;
  __u8 start_guest = ctx->next_sched_guest;
  const int charge_budget = comb_perf_iso(ctx);

  (void) mem_len;
  if (n_guests == 0)
    return 0;

  if (start_guest >= n_guests)
    start_guest = 0;

  has_sched_work = 0;
  for (i = 0; i < n_guests; i++)
  {
    g = &ctx->guests[i];
    if (!g->proto.has_event_sched)
      continue;

    if (sched_head(&g->proto.ebpf_ctx.sched) == NULL)
      continue;

    if (charge_budget && __atomic_load_n(g->budget, __ATOMIC_RELAXED) <= 0)
      continue;

    has_sched_work = 1;
    break;
  }

  if (!has_sched_work)
    return 0;

  max = FAST_SCHED_BATCH_SIZE;
  if (TXBUF_SIZE - ctx->tx_n < max)
    max = TXBUF_SIZE - ctx->tx_n;

  max = txcache_alloc(ctx, &mbs, max);
  if (max == 0)
    return 0;

  rte_prefetch0(rte_pktmbuf_mtod(mbs[0], __u8 *));
  rte_prefetch0(rte_pktmbuf_mtod(mbs[0], __u8 *) + 64);

  ntx = 0;
  last_sched_guest = -1;
  for (i = 0; i < n_guests && ntx < max; i++)
  {
    if (ntx + 1 < max)
    {
      rte_prefetch0(rte_pktmbuf_mtod(mbs[ntx + 1], __u8 *));
      rte_prefetch0(rte_pktmbuf_mtod(mbs[ntx + 1], __u8 *) + 64);
    }

    gid = (start_guest + i) % n_guests;
    ret = sched_poll_slot(ctx, gid, mbs, max, &ntx, charge_budget);
    if (ret < 0)
      return (uint64_t) -1;
    if (ret > 0)
      last_sched_guest = gid;
  }

  if (last_sched_guest >= 0)
    ctx->next_sched_guest = (last_sched_guest + 1) % n_guests;

  txcache_unalloc(ctx, max - ntx);
  return ntx;
}

static inline int rx_poll_slot(struct fast_context *ctx,
    struct guest_fast *g, struct rte_mbuf *mb, __u64 pkt_off,
    __u64 tsc_start, int charge_budget)
{
  switch (g->id)
  {
    case 0:
      return rx_poll_slot_0(ctx, g, mb, pkt_off, tsc_start, charge_budget);
    case 1:
      return rx_poll_slot_1(ctx, g, mb, pkt_off, tsc_start, charge_budget);
    case 2:
      return rx_poll_slot_2(ctx, g, mb, pkt_off, tsc_start, charge_budget);
    case 3:
      return rx_poll_slot_3(ctx, g, mb, pkt_off, tsc_start, charge_budget);
    default:
      return 0;
  }
}

static inline int sched_poll_slot(struct fast_context *ctx, int slot,
    struct rte_mbuf **mbs, int max, int *ntx, int charge_budget)
{
  switch (slot)
  {
    case 0:
      return sched_poll_slot_0(ctx, mbs, max, ntx, charge_budget);
    case 1:
      return sched_poll_slot_1(ctx, mbs, max, ntx, charge_budget);
    case 2:
      return sched_poll_slot_2(ctx, mbs, max, ntx, charge_budget);
    case 3:
      return sched_poll_slot_3(ctx, mbs, max, ntx, charge_budget);
    default:
      return 0;
  }
}

static inline int deq_poll_slot(struct fast_context *ctx, int slot,
    struct rte_mbuf **mbs, int max, int *ntx, int *ndeq, int charge_budget)
{
  switch (slot)
  {
    case 0:
      return deq_poll_slot_0(ctx, mbs, max, ntx, ndeq, charge_budget);
    case 1:
      return deq_poll_slot_1(ctx, mbs, max, ntx, ndeq, charge_budget);
    case 2:
      return deq_poll_slot_2(ctx, mbs, max, ntx, ndeq, charge_budget);
    case 3:
      return deq_poll_slot_3(ctx, mbs, max, ntx, ndeq, charge_budget);
    default:
      return 0;
  }
}

static inline void dqueue_rotate_head(struct guest_fast *g,
    struct cham_dqueue *qcur)
{
  g->proto.dqueues[g->proto.dqueues_tail].next = qcur->id;
  g->proto.dqueues_tail = qcur->id;

  if (g->proto.ndqueues == 1)
    g->proto.dqueues_head = qcur->id;
  else
    g->proto.dqueues_head = qcur->next;

  qcur->next = PROTOQ_ID_INVALID;
}

#include <stdlib.h>
#include <sys/mman.h>

#include <rte_malloc.h>

#include "nic_fast.h"
#include "fast.h"
#include "nic.h"
#include "nic_fast.h"
#include "queue_fns.h"
#include "queue_types.h"
#include "ip_hdr.h"
#include "eth_hdr.h"
#include "arp_hdr.h"
#include "gre_hdr.h"
#include "arp.h"
#include "log.h"
#include "log_pkt.h"
#include "config.h"
#include "controlif.h"
#include "ebpf.h"
#include "txcache.h"
#include "clock.h"
#include "udp.h"
#include "netvirt.h"


struct guest_fast * init_guest(__u8 id, __u64 shm_len);

static inline struct guest_fast * process_infra_rx(struct fast_context *ctx,
    struct rte_mbuf *mbuf, __u64 *pkt_off);
static inline int process_infra_rx_arp(struct fast_context *ctx, struct arp_pkt *pkt);
static inline struct guest_fast * process_infra_rx_ip(struct fast_context *ctx,
    struct rte_mbuf *mb, void *pkt, __u64 *pkt_off);
static inline void process_arp_rx_req(struct fast_context *ctx,
    struct queue_entry *qe, struct arp_pkt *pkt);
static inline void process_arp_rx_rep(struct fast_context *ctx,
    struct queue_entry *qe, struct arp_pkt *pkt);

static inline int process_infra_tx(struct fast_context *ctx,
    struct guest_fast *g, struct rte_mbuf *mb, size_t pkt_len);
static inline void process_infra_tx_gre(struct fast_context *ctx, 
    struct guest_fast *g, struct rte_mbuf *mb, 
    __u32 outer_remote_ip, size_t pkt_len);
static inline int process_infra_tx_arp(struct fast_context *ctx, 
    struct guest_fast *g, struct rte_mbuf *mb, __u32 outer_remote_ip);

static inline int poll_rx(struct fast_context *ctx);
static inline int poll_queues(struct fast_context *ctx);
static inline int poll_tx(struct fast_context *ctx);
static inline int tx_flush(struct fast_context *ctx);

int fast_context_init(struct fast_context *f_ctx, 
    struct nic_context *nic_ctx, __u16 thread_id,
    struct shm_handle *fc_handle, struct shm_handle *cf_handle, 
    struct shm_handle *ctxq_handle, struct configuration *config, 
    int shm_fd_internal, void *shm_base_internal)
{
  int i, j, ret;
  struct dqueue *cfq, *ctxq;
  struct equeue *fcq;
  struct guest_fast *guests;

  f_ctx->id = thread_id;
  f_ctx->config = config;
  f_ctx->shm_fd_internal = shm_fd_internal;
  f_ctx->shm_base_internal= shm_base_internal;
  nic_fast_init(nic_ctx, &f_ctx->nic_ctx, thread_id, config);

  /* Initialize ARP table with default values */
  arp_table_init(&f_ctx->arp_table);
  
  cfq = dqueue_new(config->cham_queue_len, 
      sizeof(struct queue_entry),
      cf_handle->addr, cf_handle->off);
  if (cfq == NULL)
  {
    LOG_ERROR("failed to create fast to control path queue");
    return -1;
  }
  f_ctx->ctl_fast_q = cfq;

  fcq = equeue_new(config->cham_queue_len, sizeof(struct queue_entry),
      fc_handle->addr, fc_handle->off);
  if (fcq == NULL)
  {
    LOG_ERROR("failed to create control to fast path queue");
    return -1;
  }
  f_ctx->fast_ctl_q = fcq;
  
  ctxq = dqueue_new(config->control_txq_len, config->control_txq_pkt_len,
      ctxq_handle->addr, ctxq_handle->off);
  if (ctxq == NULL)
  {
    LOG_ERROR("failed to create control tx queue");
    return -1;
  }
  f_ctx->ctl_txq = ctxq;

  guests = rte_calloc("fast path guests", config->max_guests, 
      sizeof(struct guest_fast), 0);
  if (guests == NULL)
  {
    LOG_ERROR("failed to allocate guest list");
    return -1;
  }
  f_ctx->guests = guests;

  /* Set initial ID to invalid for each scheduler entry in guest */
  for (i = 0; i < config->max_guests; i++)
  {
    for (j = 0; j < MAX_SCHED_ENTRIES; j++)
    {
      f_ctx->guests[i].proto.ebpf_ctx.sched.entries[j].id = SCHED_ID_INVALID;
    }
  }

  /* Preallocate mbufs so we don't have to do that in the critical path */
  ret = rte_pktmbuf_alloc_bulk(f_ctx->nic_ctx.pool, 
      f_ctx->tx_cache_mbs, TX_CACHE_SIZE);
  if (ret != 0)
  {
    LOG_ERROR("failed to preallocate tx cache");
    return -1;
  }
  f_ctx->tx_cache_n = TX_CACHE_SIZE;
  f_ctx->tx_cache_head = 0;

  return 0;
}

int fast_loop(struct fast_context *ctx)
{
  int ret;

  while(1) 
  {
    ret = poll_rx(ctx);
    if (ret < 0)
    {
      LOG_ERROR("poll_rx failed");
      return -1;
    }

    ret = poll_queues(ctx);
    if (ret < 0)
    {
      LOG_ERROR("poll_queues failed");
    }

    ret = poll_tx(ctx);
    if (ret < 0)
    {
      LOG_ERROR("poll_tx failed");
    }

    ret = controlif_poll(ctx);
    if (ret < 0)
    {
      LOG_ERROR("controlif_poll failed");
    }
    
    /* Flush entries in transmit buffer added by controlif_poll */
    tx_flush(ctx);
  }
}

static inline int poll_rx(struct fast_context *ctx)
{
  int i, n, ret;
  struct rte_mbuf *mbs[FAST_BATCH_SIZE];
  struct guest_fast *g;
  __u64 tsc_start, tsc_spent, pkt_off;

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
    g = process_infra_rx(ctx, mbs[i], &pkt_off);
    
    /* Execute custom protocol rx procedure */
    if (g != NULL)
    {
      /* Drop if this guest is out of budget */
      if (__atomic_load_n(g->budget, __ATOMIC_RELAXED) <= 0)
        continue;
      
      g->proto.ebpf_ctx.pkt = rte_pktmbuf_mtod(mbs[i], __u8 *) + pkt_off;
      g->proto.ebpf_ctx.pkt_end = (void *) (g->proto.ebpf_ctx.pkt + UDP_MSS);
      ebpf_vm_exec(g->proto.event_rx_vm, &g->proto.ebpf_ctx, 
          sizeof(struct cham_ebpf_ctx), &ret);
 
      /* Spend guest budget */
      tsc_spent = clock_rdtsc() - tsc_start;
      __atomic_fetch_sub(g->budget, tsc_spent, __ATOMIC_RELAXED);
    }
  }

  /* Return used mbufs to the mbuf pool */
  rte_pktmbuf_free_bulk(mbs, n);

  return n;
}

static inline int poll_queues(struct fast_context *ctx)
{
  int i, j, max, ret, deq_ret, ndeq, ntx;
  __u8 qcur_empty;
  struct guest_fast *g;
  struct cham_dqueue *qcur;
  struct queue_entry *qe;
  struct rte_mbuf **mbs;
  __u64 tsc_start, tsc_spent;

  max = FAST_BATCH_SIZE;
  if (TXBUF_SIZE - ctx->tx_n < max)
    max = TXBUF_SIZE - ctx->tx_n;

  /* Allocate mbufs for transmission */
  max = txcache_alloc(ctx, &mbs, max);
  if (max <= 0)
    return 0;

  ntx = 0;
  ndeq = 0;
  qcur_empty = 0;
  for (i = 0; i < ctx->n_guests && ndeq < max; i++)
  {
    tsc_start = clock_rdtsc();
    g = &ctx->guests[i];

    /* Continue if there are no activated queues for this protocol */
    if (g->proto.dqueues_head == PROTOQ_ID_INVALID)
      continue;

    /* Continue if this guest is out of budget */
    if (__atomic_load_n(g->budget, __ATOMIC_RELAXED) <= 0)
      continue;

    for (j = 0; j < g->proto.ndqueues; j++)
    {
      qcur_empty = 0;
      qcur = &g->proto.dqueues[g->proto.dqueues_head];
      qe = queue_head(&qcur->dq);

      /* If there are no messages in queue, continue to next */
      if (qe == NULL)
      {
        g->proto.dqueues[g->proto.dqueues_tail].next = qcur->id;
        g->proto.dqueues_tail = qcur->id;
        g->proto.dqueues_head = qcur->next;
        qcur->next = PROTOQ_ID_INVALID;
        qcur_empty = 1;
        continue;
      }

      /* Prepare packet buffer for potential TX */
      mbs[ntx]->data_off = 0;
      g->proto.ebpf_ctx.pkt = rte_pktmbuf_mtod(mbs[ntx], __u8 *) + 
          sizeof(struct eth_hdr);
      if (ctx->config->virt_gre)
      {
        g->proto.ebpf_ctx.pkt = g->proto.ebpf_ctx.pkt + 
          sizeof(struct ip_hdr) + sizeof(struct gre_hdr);
      }
      g->proto.ebpf_ctx.pkt_end = (void *) ((__u64) rte_pktmbuf_mtod(mbs[ntx], __u8 *) + UDP_MSS);

      /* Add queue entry to eBPF context */
      g->proto.ebpf_ctx.qe = qe;
      g->proto.ebpf_ctx.qid = qcur->id;

      /* Execute custom dequeue procedure */
      ebpf_vm_exec(g->proto.event_deq_vm, &g->proto.ebpf_ctx,
        sizeof(struct cham_ebpf_ctx), &deq_ret);
      ndeq++;

      /* Add to transmission buffer if packet processed for TX */
      if (deq_ret > 0)
      {
        /* Add destination MAC address and run infra processing */
        ret = process_infra_tx(ctx, g, mbs[ntx], deq_ret);

        /* Add to TX buffer if infra protos were successful */
        if (ret == 0)
        {
          ctx->tx_mbs[ctx->tx_n] = mbs[ntx];
          ctx->tx_n++;
          ntx++;
        }
      }

      ret = queue_dequeue(&qcur->dq);
      if (ret != 0)
      {
        LOG_ERROR("failed to dequeue queue for proto=%d", g->id);
        return -1;
      }
    }

    /* Rotate head if last polled queue didn't increment head */
    if (!qcur_empty)
    {
      qcur = &g->proto.dqueues[g->proto.dqueues_head];
      g->proto.dqueues[g->proto.dqueues_tail].next = qcur->id;
      g->proto.dqueues_tail = qcur->id;
      g->proto.dqueues_head = qcur->next;
      qcur->next = PROTOQ_ID_INVALID;
    }

    /* Subtract from guest's budget */
    tsc_spent = clock_rdtsc() - tsc_start;
    __atomic_fetch_sub(g->budget, tsc_spent, __ATOMIC_RELAXED);
  }

  tx_flush(ctx);

  /* Free buffers that were not used */
  for (i = 0; i < max; i++)
    txcache_free(ctx, mbs[i]);

  return 0;
}

static inline int poll_tx(struct fast_context *ctx)
{
  unsigned max;
  int i, tx_ret, ntx, ret;
  struct guest_fast *g;
  struct rte_mbuf **mbs;
  __u64 tsc_start, tsc_spent;
  __u8 n_guests = ctx->n_guests;
  
  /* Return if no guests have registered */
  if (ctx->guests == NULL)
    return 0;

  max = FAST_BATCH_SIZE;
  if (TXBUF_SIZE - ctx->tx_n < max)
    max = TXBUF_SIZE - ctx->tx_n;

  /* Allocate mbufs to use for transmission from mempool cache */
  max = txcache_alloc(ctx, &mbs, max);

  g = ctx->guests;
  ntx = 0;
  for (i = 0; i < n_guests && g != NULL && ntx < max; i++)
  {
    tsc_start = clock_rdtsc();
    g = &ctx->guests[i];

    /* Continue to next guest if ebpf code hasn't been uploaded yet */
    if (g->proto.event_tx_vm == NULL)
      continue;
    
    /* Continue to next guest if out of budget */
    if (__atomic_load_n(g->budget, __ATOMIC_RELAXED) <= 0)
      continue;

    for (;ntx < max;)
    {
      /* Prepare packet */
      mbs[ntx]->data_off = 0;
      g->proto.ebpf_ctx.pkt = rte_pktmbuf_mtod(mbs[ntx], __u8 *) + 
          sizeof(struct eth_hdr);
      if (ctx->config->virt_gre)
      {
        g->proto.ebpf_ctx.pkt = g->proto.ebpf_ctx.pkt +
          sizeof(struct ip_hdr) + sizeof(struct gre_hdr);
      }
      g->proto.ebpf_ctx.pkt_end = (void *) ((__u64) rte_pktmbuf_mtod(mbs[ntx], __u8 *) + UDP_MSS);
      
      /* Execute custom protocol tx procedure */
      ebpf_vm_exec(g->proto.event_tx_vm, &g->proto.ebpf_ctx.pkt, 
          sizeof(struct cham_ebpf_ctx), &tx_ret);

      if (tx_ret >= 0)
      {
        /* Add destination MAC address */
        ret = process_infra_tx(ctx, g, mbs[ntx], tx_ret);
            
        /* TODO: Don't drop packet if ARP lookup hasn't resolved */
        if (ret == 0)
        {
          /* Add to transmission buffer if packet processed for TX */
          ctx->tx_mbs[ctx->tx_n] = mbs[ntx];
          ctx->tx_n++;
          ntx++;
        }
      }
      else
      {
        break;
      }
    }

    /* Subtract guest's budget */
    tsc_spent = clock_rdtsc() - tsc_start;
    __atomic_fetch_sub(g->budget, tsc_spent, __ATOMIC_RELAXED);
  }

  tx_flush(ctx);

  /* Free buffers that were not used */
  for (i = 0; i < max; i++)
    txcache_free(ctx, mbs[i]);

  return 0;
}

static inline int tx_flush(struct fast_context *ctx)
{
  int i, ret;

  /* Push packets to the NIC */
  ret = nic_fast_tx(&ctx->nic_ctx, ctx->tx_n, ctx->tx_mbs);

  if (ret == ctx->tx_n)
  {
    /* Everything sent */
    ctx->tx_n = 0;
  }
  else if (ret > 0)
  {
    /* Move unsent packets to front */
    for (i = ret; i < ctx->tx_n; i++)
      ctx->tx_mbs[i - ret] = ctx->tx_mbs[i];

    ctx->tx_n -= ret;
  }

  return ret;
}

static inline struct guest_fast * process_infra_rx(struct fast_context *ctx,
    struct rte_mbuf *mb, __u64 *pkt_off)
{
  __u16 eth_type;
  struct eth_hdr *eth;
  
  eth = (struct eth_hdr *) rte_pktmbuf_mtod(mb, __u8 *);
  eth_type = f_beui16(eth->type);
  
  /* Send ARP packet to control */
  switch (eth_type)
  {
    case ETH_TYPE_ARP:
      process_infra_rx_arp(ctx, (struct arp_pkt *) eth);
      return NULL;
    default:
      return process_infra_rx_ip(ctx, mb, eth, pkt_off);
  }
}

static inline int process_infra_rx_arp(struct fast_context *ctx, struct arp_pkt *pkt)
{
  struct queue_entry *qe;

  qe = queue_tail(ctx->fast_ctl_q);
  if (qe == NULL)
  {
    LOG_ERROR("failed to get tail from fast->control queue");
    return -1;
  }
  
  if (f_beui16(pkt->arp.oper) == ARP_OPER_REQUEST)
    process_arp_rx_req(ctx, qe, pkt);
  else if (f_beui16(pkt->arp.oper) == ARP_OPER_REPLY)
    process_arp_rx_rep(ctx, qe, pkt);
  
  return 0;
}

static inline struct guest_fast * process_infra_rx_ip(struct fast_context *ctx,
    struct rte_mbuf *mb, void *pkt, __u64 *pkt_off)
{
  struct gre_pkt *pkt_gre;
  struct netvirt_entry *e;

  if (ctx->config->virt_gre)
  {
    pkt_gre = pkt;
    e = netvirt_table_get(ctx->inner_table, 
        f_beui32(pkt_gre->gre.key), f_beui32(pkt_gre->inner_ip.dst));
    if (e == NULL)
    {
      LOG_WARN("received packet for unkown gueset");
      return NULL;
    }
    *pkt_off = sizeof(struct eth_hdr) + sizeof(struct ip_hdr) + sizeof(struct gre_hdr);
    return &ctx->guests[e->gid];
  }
  else
  {
    *pkt_off = sizeof(struct eth_hdr);
    return &ctx->guests[0];
  }
}

static inline int process_infra_tx(struct fast_context *ctx, 
    struct guest_fast *g, struct rte_mbuf *mb, size_t pkt_len)
{
  int ret;
  __u32 outer_remote_ip;
  struct ip_hdr *ip;
  struct netvirt_entry *e;
  void *pkt;
  struct gre_pkt *gre_pkt;

  pkt = rte_pktmbuf_mtod(mb, __u8 *);
  if (ctx->config->virt_gre)
  {
    gre_pkt = pkt;
    e = netvirt_table_get(ctx->inner_table, g->gre_key, 
        f_beui32(gre_pkt->inner_ip.dst));
    if (e == NULL)
    {
      LOG_WARN("could not find outer ip for destination");
      return -1;
    }
    outer_remote_ip = e->outer_ip;
  }
  else
  {
    ip = (struct ip_hdr *) (pkt + sizeof(struct eth_hdr));
    outer_remote_ip = f_beui32(ip->dst);
  }

  ret = process_infra_tx_arp(ctx, g, mb, outer_remote_ip);
  if (ret != 0)
    return -1;

  if (ctx->config->virt_gre)
  {
    process_infra_tx_gre(ctx, g, mb, outer_remote_ip, pkt_len);
  }
  else
  {
    mb->pkt_len = mb->data_len = pkt_len + sizeof(struct eth_hdr);
    mb->ol_flags |= RTE_MBUF_F_TX_IPV4 | 
        RTE_MBUF_F_TX_IP_CKSUM | RTE_MBUF_F_TX_UDP_CKSUM;
  }

  return 0;
}

static inline int process_infra_tx_arp(struct fast_context *ctx, 
    struct guest_fast *g, struct rte_mbuf *mb, __u32 outer_remote_ip)
{
  int ret;
  struct eth_hdr *eth;
  struct arp_entry *ae;
  struct queue_entry *qe;
  void *pkt;
  
  pkt = rte_pktmbuf_mtod(mb, __u8 *);
  eth = (struct eth_hdr *) pkt;

  /* Find dst MAC address for IP */
  ae = arp_lookup(&ctx->arp_table, outer_remote_ip);
  if (ae == NULL)
  {
    /* ARP entry doesn't exist so send message to control path to resolve */
    qe = queue_tail(ctx->fast_ctl_q);
    if (qe == NULL)
    {
      LOG_ERROR("failed to get tail for fast->control queue");
      return -1;
    }
    
    qe->data.arp_lookup.ip = outer_remote_ip;
    ret = queue_enqueue(ctx->fast_ctl_q, QUEUE_ARP_LOOKUP);
    if (ret != 0)
    {
      LOG_ERROR("failed to enqueue ARP lookup to control");
      return -1;
    }
    
    /* Mark ARP entry as pending */
    ae = arp_insert_pending(&ctx->arp_table, outer_remote_ip);
    if (ae == NULL)
    {
      LOG_ERROR("failed to insert pending ARP entry");
      return -1;
    }
    
    return -1;
  }
  
  /* Return if ARP entry is still pending */
  if (ae->pending)
    return -1;
  
  /* Copy MAC addresses to packet */
  memcpy(eth->dst.addr, ae->mac, ETH_ADDR_LEN);
  memcpy(eth->src.addr, ctx->nic_ctx.eth_addr.addr_bytes, ETH_ADDR_LEN);

  /* Set type of next header */
  eth->type = t_beui16(ETH_TYPE_IP);

  return 0;
}

static inline void process_infra_tx_gre(struct fast_context *ctx, 
    struct guest_fast *g, struct rte_mbuf *mb, 
    __u32 outer_remote_ip, size_t pkt_len)
{
  struct gre_pkt *pkt;

  pkt = (struct gre_pkt *) rte_pktmbuf_mtod(mb, __u8 *);

  IPH_VHL_SET(&pkt->outer_ip, 4, 5);
  pkt->outer_ip._tos = 0;
  pkt->outer_ip.len = t_beui16(pkt_len + sizeof(struct gre_hdr) 
      + sizeof(struct ip_hdr));
  pkt->outer_ip.id = t_beui16(3);
  pkt->outer_ip.offset = t_beui16(0);
  pkt->outer_ip.ttl = 0xff;
  pkt->outer_ip.proto = IP_PROTO_GRE;
  pkt->outer_ip.chksum = 0;
  pkt->outer_ip.src = t_beui32(ctx->config->ip);
  pkt->outer_ip.dst = t_beui32(outer_remote_ip);

  GREH_CKSV_SET(&pkt->gre, 0, 1, 0, 0);
  pkt->gre.proto = t_beui16(GRE_PROTO_IP);
  pkt->gre.key = t_beui32(g->gre_key);

  mb->pkt_len = mb->data_len = pkt_len + sizeof(struct eth_hdr) + sizeof(struct ip_hdr) + sizeof(struct gre_hdr);
  /* Enable checksum offload */
  // mb->l2_len = 0;
  // mb->l3_len = sizeof(struct ip_hdr);
  // mb->l4_len = 0;
  // mb->outer_l2_len = sizeof(struct eth_hdr);
  // mb->outer_l3_len = sizeof(struct ip_hdr);
  // mb->ol_flags = RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_IP_CKSUM |
  //   RTE_MBUF_F_TX_OUTER_IPV4 | RTE_MBUF_F_TX_OUTER_IP_CKSUM  |
  //   RTE_MBUF_F_TX_TCP_CKSUM | RTE_MBUF_F_TX_TUNNEL_GRE;
}

static inline void process_arp_rx_req(struct fast_context *ctx,
    struct queue_entry *qe, struct arp_pkt *pkt)
{
  int ret;
  
  qe->data.arp_pkt_rx_req.spa = f_beui32(pkt->arp.spa);
  qe->data.arp_pkt_rx_req.tpa = f_beui32(pkt->arp.tpa);
  rte_memcpy(&qe->data.arp_pkt_rx_req.sha, &pkt->arp.sha, ETH_ADDR_LEN);
  
  ret = queue_enqueue(ctx->fast_ctl_q, QUEUE_ARP_RX_REQ);
  if (ret != 0)
  {
    LOG_ERROR("ARP request RX enqueue to fast->control failed");
  }
}

static inline void process_arp_rx_rep(struct fast_context *ctx,
    struct queue_entry *qe, struct arp_pkt *pkt)
{
  int ret;
  
  qe->data.arp_pkt_rx_rep.spa = f_beui32(pkt->arp.spa);
  qe->data.arp_pkt_rx_rep.tpa = f_beui32(pkt->arp.tpa);
  rte_memcpy(&qe->data.arp_pkt_rx_rep.sha, &pkt->arp.sha, ETH_ADDR_LEN);
  rte_memcpy(&qe->data.arp_pkt_rx_rep.tha, &pkt->arp.tha, ETH_ADDR_LEN);
  
  ret = queue_enqueue(ctx->fast_ctl_q, QUEUE_ARP_RX_REP);
  if (ret != 0)
  {
    LOG_ERROR("ARP reply RX enqueue to fast->control failed");
  }
}


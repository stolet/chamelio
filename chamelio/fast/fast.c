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
#include "arp.h"
#include "udp.h"
#include "log.h"
#include "config.h"
#include "controlif.h"
#include "ebpf.h"
#include "txcache.h"


struct guest_fast * init_guest(__u8 id, __u64 shm_len);

static inline struct guest_fast * process_infra_rx(struct fast_context *ctx, struct rte_mbuf *mbuf);
static inline int process_infra_tx(struct fast_context *ctx, struct rte_mbuf *mb);
static inline void process_arp_rx_req(struct fast_context *ctx,
    struct queue_entry *qe, struct pkt_arp *pkt);
static inline void process_arp_rx_rep(struct fast_context *ctx,
    struct queue_entry *qe, struct pkt_arp *pkt);

int poll_rx(struct fast_context *ctx);
int poll_queues(struct fast_context *ctx);
int poll_tx(struct fast_context *ctx);
int poll_control(struct fast_context *ctx);
int tx_flush(struct fast_context *ctx);

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

    /* Flush entries in transmit buffer added by poll_queues */
    tx_flush(ctx);

    ret = poll_tx(ctx);
    if (ret < 0)
    {
      LOG_ERROR("poll_tx failed");
    }

    /* Flush entries in transmit buffer added by poll_tx */
    tx_flush(ctx);

    ret = poll_control(ctx);
    if (ret < 0)
    {
      LOG_ERROR("poll_control failed");
    }
    
    /* Flush entries in transmit buffer added by poll_control */
    tx_flush(ctx);
  }
}

int poll_rx(struct fast_context *ctx)
{
  int i, n, ret;
  struct rte_mbuf *mbs[BATCH_SIZE];
  struct guest_fast *g;

  n = BATCH_SIZE;
  if (TXBUF_SIZE - ctx->tx_n < n)
    n = TXBUF_SIZE - ctx->tx_n;

  /* Receive packets from the NIC */
  n = nic_fast_rx(&ctx->nic_ctx, n, mbs);

  if (n <= 0)
    return 0;

  for (i = 0; i < n; i++)
  {
    /* Process infrastructure protocols */
    g = process_infra_rx(ctx, mbs[i]);

    if (g != NULL)
    {
      // g->proto.event_rx(rte_pktmbuf_mtod(mbs[i], __u8 *), 
          // &g->proto.handle);
      g->proto.ebpf_ctx.pkt = rte_pktmbuf_mtod(mbs[i], __u8 *);
      ebpf_vm_exec(g->proto.event_rx_vm, &g->proto.ebpf_ctx, 
          sizeof(struct cham_ebpf_ctx), &ret);
    }
  }

  rte_pktmbuf_free_bulk(mbs, n);

  return n;
}

int poll_queues(struct fast_context *ctx)
{
  int i, max, ret, deq_ret, ndeq, ntx;
  __u16 qid;
  struct guest_fast *g;
  struct cham_dqueue *q;
  struct queue_entry *qe;
  struct rte_mbuf **mbs;

  max = BATCH_SIZE;
  if (TXBUF_SIZE - ctx->tx_n < max)
    max = TXBUF_SIZE - ctx->tx_n;

  /* Allocate mbufs in case a message wants to transmit something already */
  max = txcache_alloc(ctx, &mbs, max);

  ntx = ndeq = 0;
  for (i = 0; i < ctx->n_guests && ndeq < max; i++)
  {
    g = &ctx->guests[i];
    qid = g->proto.dqueues_head;
    while (qid != PROTOQ_ID_INVALID && ndeq < max)
    {
      q = &g->proto.dqueues[qid];
        
      /* If there are no messages in queue continue */
      qe = queue_head(&q->dq);
      if (qe == NULL)
      {
        qid = q->next;
        continue;
      }

      /* Prepare packet */
      mbs[ntx]->data_off = 0;
      g->proto.ebpf_ctx.pkt = rte_pktmbuf_mtod(mbs[ntx], __u8 *);

      /* Add queue entry to ebpf context */
      g->proto.ebpf_ctx.qe = qe;
      g->proto.ebpf_ctx.qid = q->id;

      /* Execute custom dequeue procedure */
      ebpf_vm_exec(g->proto.event_deq_vm, &g->proto.ebpf_ctx, 
        sizeof(struct cham_ebpf_ctx), &deq_ret);
      ndeq++;

      /* Add to transmission buffer if packet processed for TX */
      if (deq_ret > 0)
      {
        /* Add destination MAC address */
        process_infra_tx(ctx, mbs[ntx]);
        
        /* Add to TX buffer if infra protos were successful */
        if (ret == 0)
        {
          mbs[ntx]->pkt_len = mbs[ntx]->data_len = deq_ret;
          ctx->tx_mbs[ctx->tx_n] = mbs[ntx];
          ctx->tx_n++;
          ntx++;
        }
      }
      
      // g->proto.event_deq(q->id, qe, &g->proto.handle);

      /* Pop the queue */
      ret = queue_dequeue(&q->dq);
      if (ret != 0)
      {
        LOG_ERROR("failed to dequeue queue for proto=%d", g->id);
        return -1;
      }

      qid = q->next;
    }
  }

  /* Free buffers that were not used */
  for (i = ntx; i < max; i++)
    txcache_free(ctx, mbs[i]);

  return 0;
}

int poll_tx(struct fast_context *ctx)
{
  unsigned max;
  int i, tx_ret, ntx, ret;
  struct guest_fast *g;
  struct rte_mbuf **mbs;
  __u8 n_guests = ctx->n_guests;
  
  if (ctx->guests == NULL)
    return 0;

  max = BATCH_SIZE;
  if (TXBUF_SIZE - ctx->tx_n < max)
    max = TXBUF_SIZE - ctx->tx_n;

  /* Allocate mbufs to use for transmission from mempool cache */
  max = txcache_alloc(ctx, &mbs, max);

  g = ctx->guests;
  ntx = 0;
  for (i = 0; i < n_guests && g != NULL && ntx < max; i++)
  {
    g = &ctx->guests[i];
    if (g->proto.event_tx_vm == NULL)
      continue;
    
    for (;ntx < max;)
    {
      /* Prepare packet */
      mbs[ntx]->data_off = 0;
      g->proto.ebpf_ctx.pkt = rte_pktmbuf_mtod(mbs[ntx], __u8 *);
      // ret = g->proto.event_tx(rte_pktmbuf_mtod(mbs[n_used], __u8 *), 
          // &guest->proto.handle);
      
      /* Execute custom TX procedure */
      ebpf_vm_exec(g->proto.event_tx_vm, &g->proto.ebpf_ctx.pkt, 
          sizeof(struct cham_ebpf_ctx), &tx_ret);

      if (tx_ret >= 0)
      {
        /* Add destination MAC address */
        ret = process_infra_tx(ctx, mbs[ntx]);

        if (ret == 0)
        {
          /* Add to transmission buffer if packet processed for TX */
          mbs[ntx]->pkt_len = mbs[ntx]->data_len = tx_ret;
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
  }

  /* Free buffers that were not used */
  for (i = ntx; i < max; i++)
    txcache_free(ctx, mbs[i]);

  return 0;
}

int tx_flush(struct fast_context *ctx)
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

int poll_control(struct fast_context *ctx)
{
  return controlif_poll(ctx);
}

static inline struct guest_fast * process_infra_rx(struct fast_context *ctx, struct rte_mbuf *mb)
{
  __u16 eth_type;
  struct pkt_arp *pkt;
  struct queue_entry *qe;
  
  pkt = (struct pkt_arp *) rte_pktmbuf_mtod(mb, __u8 *);
  eth_type = f_beui16(pkt->eth.type);
  
  /* Send ARP packet to control */
  if (eth_type == ETH_TYPE_ARP)
  {
    qe = queue_tail(ctx->fast_ctl_q);
    if (qe == NULL)
    {
      LOG_ERROR("failed to get tail from fast->control queue");
      return NULL;
    }
    
    if (f_beui16(pkt->arp.oper) == ARP_OPER_REQUEST)
      process_arp_rx_req(ctx, qe, pkt);
    else if (f_beui16(pkt->arp.oper) == ARP_OPER_REPLY)
      process_arp_rx_rep(ctx, qe, pkt);
    
    return NULL;
  }
  
  /* TODO: Use GRE headers to identify guest and protocol */
  return &ctx->guests[0];
}

static inline int process_infra_tx(struct fast_context *ctx, struct rte_mbuf *mb)
{
  int ret;
  struct eth_hdr *eth;
  struct ip_hdr  *ip;
  struct arp_entry *ae;
  struct queue_entry *qe;
  void *pkt;
  
  pkt = rte_pktmbuf_mtod(mb, __u8 *);
  eth = (struct eth_hdr *) pkt;
  ip = (struct ip_hdr *) ((__u8 *) pkt + sizeof(struct eth_hdr));

  /* Find dst MAC address for IP */
  ae = arp_lookup(&ctx->arp_table, f_beui32(ip->dst));
  if (ae == NULL)
  {
    /* ARP entry doesn't exist so send message to control path to resolve */
    qe = queue_tail(ctx->fast_ctl_q);
    if (qe == NULL)
    {
      LOG_ERROR("failed to get tail for fast->control queue");
      return -1;
    }
    
    qe->data.arp_lookup.ip = f_beui32(ip->dst);
    ret = queue_enqueue(ctx->fast_ctl_q, QUEUE_ARP_LOOKUP);
    if (ret != 0)
    {
      LOG_ERROR("failed to enqueue ARP lookup to control");
      return -1;
    }
    
    /* Mark ARP entry as pending */
    arp_insert_pending(&ctx->arp_table, f_beui32(ip->dst));
    
    return -1;
  }
  
  /* Return if ARP entry is still pending */
  if (ae->pending)
    return -1;
  
  /* Copy MAC addresses to packet */
  memcpy(eth->dst.addr, ae->mac, ETH_ADDR_LEN);
  memcpy(eth->src.addr, ctx->nic_ctx.eth_addr.addr_bytes, ETH_ADDR_LEN);

  return 0;
}

static inline void process_arp_rx_req(struct fast_context *ctx,
    struct queue_entry *qe, struct pkt_arp *pkt)
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
    struct queue_entry *qe, struct pkt_arp *pkt)
{
  int ret;
  
  qe->data.arp_pkt_rx_rep.spa = f_beui32(pkt->arp.spa);
  rte_memcpy(&qe->data.arp_pkt_rx_rep.sha, &pkt->arp.sha, ETH_ADDR_LEN);
  
  ret = queue_enqueue(ctx->fast_ctl_q, QUEUE_ARP_RX_REP);
  if (ret != 0)
  {
    LOG_ERROR("ARP reply RX enqueue to fast->control failed");
  }
}


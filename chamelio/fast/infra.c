#include "infra.h"
#include <stdlib.h>
#include <linux/types.h>

#include <rte_ip.h>

#include "fast.h"
#include "netvirt.h"
#include "arp.h"
#include "eth_hdr.h"
#include "ip_hdr.h"
#include "tcp_hdr.h"
#include "udp_hdr.h"
#include "gre_hdr.h"
#include "arp_hdr.h"
#include "queue_types.h"
#include "queue_fns.h"
#include "log.h"
#include "log_pkt.h"

static inline int process_rx_arp(struct fast_context *ctx, struct arp_pkt *pkt);
static inline struct guest_fast * process_rx_ip(struct fast_context *ctx,
    struct rte_mbuf *mb, void *pkt, __u64 *pkt_off);
static inline void process_arp_rx_req(struct fast_context *ctx,
    struct queue_entry *qe, struct arp_pkt *pkt);
static inline void process_arp_rx_rep(struct fast_context *ctx,
    struct queue_entry *qe, struct arp_pkt *pkt);

static inline void process_tx_mbuf(struct rte_mbuf *mb, size_t pkt_len);
static inline void process_tx_chksum(struct rte_mbuf *mb, void *pkt);
static inline void process_tx_hdrs_gre(struct fast_context *ctx, 
    struct guest_fast *g, struct rte_mbuf *mb, 
    __u32 outer_remote_ip, size_t pkt_len);
static inline void process_tx_mbuf_gre(struct rte_mbuf *mb, size_t pkt_len);
static inline void process_tx_chksum_gre(struct rte_mbuf *mb);
static inline int process_tx_arp(struct fast_context *ctx, 
  struct guest_fast *g, size_t pkt_len,
  struct rte_mbuf *mb, __u32 outer_remote_ip);

struct guest_fast * infra_rx(struct fast_context *ctx,
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
      LOG_PKT_DEBUG(log_arp_pkt, (struct arp_pkt *) eth);
      process_rx_arp(ctx, (struct arp_pkt *) eth);
      return NULL;
    default:
    {
      /* Rate-limit: log first 32 packets then every 10k to avoid fast-path spam. */
      static __u64 rx_ip_count = 0;
      __u64 n = rx_ip_count++;
      if (n < 32 || n % 10000 == 0)
      {
        struct ip_hdr *rip = (struct ip_hdr *)(((__u8 *)eth) + sizeof(struct eth_hdr));
        LOG_DEBUG("infra_rx: IP packet #%llu len=%u", (unsigned long long)(n + 1), mb->pkt_len);
        LOG_PKT_DEBUG(log_eth, eth);
        LOG_PKT_DEBUG(log_ip,  rip);
        if (rip->proto == IP_PROTO_TCP)
          LOG_PKT_DEBUG(log_tcp, (struct tcp_hdr *)(((__u8 *)rip) + sizeof(struct ip_hdr)));
      }
      return process_rx_ip(ctx, mb, eth, pkt_off);
    }
  }
}

int infra_tx(struct fast_context *ctx, 
    struct guest_fast *g, struct rte_mbuf *mb, size_t pkt_len)
{
  int ret;
  __u32 outer_remote_ip;
  struct ip_hdr *ip;
  struct netvirt_entry *e;
  void *pkt;
  struct gre_pkt *gre_pkt;

  pkt = rte_pktmbuf_mtod(mb, __u8 *);
  if (ctx->virt_gre)
  {
    gre_pkt = pkt;
    __u32 _lookup_ip = f_beui32(gre_pkt->inner_ip.dst);
    e = netvirt_table_get(ctx->inner_table, g->gre_key, _lookup_ip);
    if (e == NULL)
    {
      LOG_WARN("infra_tx: could not find outer ip for destination gre_key=%u inner_ip=%08x",
          g->gre_key, _lookup_ip);
      return INFRA_RET_ERR;
    }
    outer_remote_ip = e->outer_ip;
  }
  else
  {
    ip = (struct ip_hdr *) (pkt + sizeof(struct eth_hdr));
    outer_remote_ip = f_beui32(ip->dst);
    LOG_DEBUG("infra_tx: pkt_len=%zu data_off=%u ip_dst=%08x ip_dst_bytes=%02x%02x%02x%02x",
        pkt_len, mb->data_off, outer_remote_ip,
        ((const __u8*)&ip->dst)[0], ((const __u8*)&ip->dst)[1],
        ((const __u8*)&ip->dst)[2], ((const __u8*)&ip->dst)[3]);
  }

  ret = process_tx_arp(ctx, g, pkt_len, mb, outer_remote_ip);
  if (ret != 0)
    return ret;

  if (ctx->virt_gre)
  {
    process_tx_hdrs_gre(ctx, g, mb, outer_remote_ip, pkt_len);
    process_tx_mbuf_gre(mb, pkt_len);
    process_tx_chksum_gre(mb);
    {
      const __u8 *_b = rte_pktmbuf_mtod(mb, const __u8 *);
      LOG_DEBUG("infra_tx: pre-NIC inner_ip_bytes=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x ol_flags=%lx",
          _b[42], _b[43], _b[44], _b[45], _b[46], _b[47],
          _b[48], _b[49], _b[50], _b[51], (unsigned long)mb->ol_flags);
    }
  }
  else
  {
    process_tx_mbuf(mb, pkt_len);
    process_tx_chksum(mb, pkt);
  }

  return INFRA_RET_OK;
}

static inline int process_rx_arp(struct fast_context *ctx, struct arp_pkt *pkt)
{
  struct queue_entry *qe;

  qe = queue_tail(ctx->fast_ctl_q);
  if (qe == NULL)
  {
    LOG_ERROR("failed to get tail from fast->control queue");
    return INFRA_RET_ERR;
  }
  
  if (f_beui16(pkt->arp.oper) == ARP_OPER_REQUEST)
    process_arp_rx_req(ctx, qe, pkt);
  else if (f_beui16(pkt->arp.oper) == ARP_OPER_REPLY)
    process_arp_rx_rep(ctx, qe, pkt);
  
  return INFRA_RET_OK;
}

static inline struct guest_fast * process_rx_ip(struct fast_context *ctx,
    struct rte_mbuf *mb, void *pkt, __u64 *pkt_off)
{
  struct gre_pkt *pkt_gre;
  struct netvirt_entry *e;

  if (ctx->virt_gre)
  {
    pkt_gre = pkt;
    e = netvirt_table_get(ctx->inner_table, 
        f_beui32(pkt_gre->gre.key), f_beui32(pkt_gre->inner_ip.dst));
    if (e == NULL)
    {
      LOG_WARN("received packet for unkown gueset");
      return NULL;
    }
    if (e->gid >= ctx->n_guests)
    {
      LOG_DEBUG("received packet for unregistered guest gid=%u n_guests=%u", e->gid, ctx->n_guests);
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

static inline int process_tx_arp(struct fast_context *ctx, 
    struct guest_fast *g, size_t pkt_len,
    struct rte_mbuf *mb, __u32 outer_remote_ip)
{
  int ret;
  struct eth_hdr *eth;
  struct arp_table_entry *ae;
  struct queue_entry *qe;
  void *pkt;
  
  pkt = rte_pktmbuf_mtod(mb, __u8 *);
  eth = (struct eth_hdr *) pkt;

  /* Find dst MAC address for IP */
  ae = arp_lookup(&ctx->arp_table, outer_remote_ip);
  if (ae == NULL)
  {
    /* Mark ARP entry as pending */
    ae = arp_insert_pending(&ctx->arp_table, outer_remote_ip);
    if (ae == NULL)
    {
      LOG_ERROR("failed to insert pending ARP entry");
      return INFRA_RET_ERR;
    }

    /* ARP entry doesn't exist so send message to control path to resolve */
    qe = queue_tail(ctx->fast_ctl_q);
    if (qe == NULL)
    {
      LOG_ERROR("failed to get tail for fast->control queue");
      return INFRA_RET_ERR;
    }
    
    qe->data.arp_lookup.ip = outer_remote_ip;
    ret = queue_enqueue(ctx->fast_ctl_q, QUEUE_ARP_LOOKUP);
    if (ret != 0)
    {
      LOG_ERROR("failed to enqueue ARP lookup to control");
    }

    qe = queue_tail(ctx->fast_ctl_q);
    if (qe == NULL)
    {
      LOG_ERROR("failed to get tail for fast->control queue");
      return INFRA_RET_ERR;
    }

    qe->data.mbuf_pending.ip = outer_remote_ip;
    qe->data.mbuf_pending.mb = mb;
    qe->data.mbuf_pending.pkt_len = pkt_len;
    qe->data.mbuf_pending.gid = g->id;
    ret = queue_enqueue(ctx->fast_ctl_q, QUEUE_MBUF_PENDING);
    if (ret != 0)
    {
      LOG_ERROR("failed to enqueue pending mbuf to control");
      return INFRA_RET_ERR;
    }

    return INFRA_RET_MBUF;
  }
  
  /* Return if ARP entry is still pending */
  if (ae->pending)
  {
    qe = queue_tail(ctx->fast_ctl_q);
    if (qe == NULL)
    {
      LOG_ERROR("failed to get tail for fast->control queue");
      return INFRA_RET_ERR;
    }

    qe->data.mbuf_pending.ip = outer_remote_ip;
    qe->data.mbuf_pending.mb = mb;
    qe->data.mbuf_pending.pkt_len = pkt_len;
    qe->data.mbuf_pending.gid = g->id;
    ret = queue_enqueue(ctx->fast_ctl_q, QUEUE_MBUF_PENDING);
    if (ret != 0)
    {
      LOG_ERROR("failed to enqueue pending mbuf to control");
      return INFRA_RET_ERR;
    }

    return INFRA_RET_MBUF;
  }
  
  /* Copy MAC addresses to packet */
  memcpy(eth->dst.addr, ae->mac, ETH_ADDR_LEN);
  memcpy(eth->src.addr, ctx->nic_ctx.eth_addr.addr_bytes, ETH_ADDR_LEN);

  /* Set type of next header */
  eth->type = t_beui16(ETH_TYPE_IP);

  return INFRA_RET_OK;
}

static inline void process_tx_mbuf(struct rte_mbuf *mb, size_t pkt_len)
{
    mb->pkt_len  = pkt_len + sizeof(struct eth_hdr);
    mb->data_len = mb->pkt_len;
    mb->l2_len   = sizeof(struct eth_hdr);
    mb->l3_len   = sizeof(struct ip_hdr);
    mb->ol_flags = 0;
}

static inline void process_tx_chksum(struct rte_mbuf *mb, void *pkt)
{
  struct ip_hdr *ip;
  struct tcp_hdr *tcp;
  struct udp_hdr *udp;

  ip = (struct ip_hdr *) (pkt + sizeof(struct eth_hdr));
  ip->chksum = 0;

  switch (ip->proto) 
  {
    case IP_PROTO_TCP:
      tcp = (struct tcp_hdr *) ((uint8_t *) ip + sizeof(struct ip_hdr));
      tcp->chksum = 0;
      mb->ol_flags = RTE_MBUF_F_TX_IPV4 |
          RTE_MBUF_F_TX_IP_CKSUM | RTE_MBUF_F_TX_TCP_CKSUM;
      tcp->chksum = rte_ipv4_phdr_cksum((struct rte_ipv4_hdr *) ip, 
          mb->ol_flags);
      break;
    case IP_PROTO_UDP:
      udp = (struct udp_hdr *) ((uint8_t *) ip + sizeof(struct ip_hdr));
      udp->chksum = 0;
      mb->ol_flags = RTE_MBUF_F_TX_IPV4 |
          RTE_MBUF_F_TX_IP_CKSUM | RTE_MBUF_F_TX_UDP_CKSUM;
      udp->chksum = rte_ipv4_phdr_cksum((struct rte_ipv4_hdr *) ip,
          mb->ol_flags);
      break;
    default:
      LOG_WARN("Got unknown type in ip header");
      break;
  }
}

static inline void process_tx_mbuf_gre(struct rte_mbuf *mb, size_t pkt_len)
{
  mb->pkt_len = mb->data_len = pkt_len + sizeof(struct eth_hdr) + 
      sizeof(struct ip_hdr) + sizeof(struct gre_hdr);
  mb->l2_len = sizeof(struct gre_hdr);
  mb->l3_len = sizeof(struct ip_hdr);
  mb->l4_len = 0;
  mb->outer_l2_len = sizeof(struct eth_hdr);
  mb->outer_l3_len = sizeof(struct ip_hdr);
  mb->ol_flags = 0;
}

static inline void process_tx_chksum_gre(struct rte_mbuf *mb)
{
  struct gre_pkt *pkt;
  struct tcp_hdr *tcp;
  struct udp_hdr *udp;

  pkt = (struct gre_pkt *) rte_pktmbuf_mtod(mb, __u8 *);
  pkt->outer_ip.chksum = 0;
  pkt->inner_ip.chksum = 0;

  switch (pkt->inner_ip.proto)
  {
    case IP_PROTO_TCP:
      tcp = (struct tcp_hdr *) ((__u8 *) &pkt->inner_ip + 
          sizeof(struct ip_hdr));
          
      tcp->chksum = 0;
      tcp->chksum = rte_ipv4_phdr_cksum((struct rte_ipv4_hdr *) &pkt->inner_ip,
          mb->ol_flags);
      mb->ol_flags = RTE_MBUF_F_TX_OUTER_IPV4 |
          RTE_MBUF_F_TX_OUTER_IP_CKSUM |
          RTE_MBUF_F_TX_TUNNEL_GRE |
          RTE_MBUF_F_TX_IPV4 |
          RTE_MBUF_F_TX_IP_CKSUM |
          RTE_MBUF_F_TX_TCP_CKSUM;
      break;
    case IP_PROTO_UDP:
      udp = (struct udp_hdr *) ((__u8 *) &pkt->inner_ip +
          sizeof(struct ip_hdr));
      udp->chksum = 0;
      udp->chksum = rte_ipv4_phdr_cksum(
          (struct rte_ipv4_hdr *) &pkt->inner_ip, mb->ol_flags);
      mb->ol_flags = RTE_MBUF_F_TX_OUTER_IPV4 |
          RTE_MBUF_F_TX_OUTER_IP_CKSUM |
          RTE_MBUF_F_TX_TUNNEL_GRE |
          RTE_MBUF_F_TX_IPV4 |
          RTE_MBUF_F_TX_IP_CKSUM |
          RTE_MBUF_F_TX_UDP_CKSUM;
      break;
    default:
      LOG_WARN("Got unknown inner type in ip header");
      break;
  }
}

static inline void process_tx_hdrs_gre(struct fast_context *ctx, 
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

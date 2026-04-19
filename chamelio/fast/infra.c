#include <stdlib.h>
#include <linux/types.h>

#include <rte_ip.h>

#include "fast.h"
#include "netvirt.h"
#include "arp.h"
#include "eth_hdr.h"
#include "ip_hdr.h"
#include "udp_hdr.h"
#include "gre_hdr.h"
#include "arp_hdr.h"
#include "queue_types.h"
#include "queue_fns.h"
#include "log.h"

static inline int process_rx_arp(struct fast_context *ctx, struct arp_pkt *pkt);
static inline struct guest_fast * process_rx_ip(struct fast_context *ctx,
    struct rte_mbuf *mb, void *pkt, __u64 *pkt_off);
static inline void process_arp_rx_req(struct fast_context *ctx,
    struct queue_entry *qe, struct arp_pkt *pkt);
static inline void process_arp_rx_rep(struct fast_context *ctx,
    struct queue_entry *qe, struct arp_pkt *pkt);

static inline void process_tx_gre(struct fast_context *ctx, 
    struct guest_fast *g, struct rte_mbuf *mb, 
    __u32 outer_remote_ip, size_t pkt_len);
static inline int process_tx_arp(struct fast_context *ctx, 
    struct guest_fast *g, struct rte_mbuf *mb, __u32 outer_remote_ip);

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
      process_rx_arp(ctx, (struct arp_pkt *) eth);
      return NULL;
    default:
      return process_rx_ip(ctx, mb, eth, pkt_off);
  }
}

int infra_tx(struct fast_context *ctx, 
    struct guest_fast *g, struct rte_mbuf *mb, size_t pkt_len)
{
  int ret;
  __u32 outer_remote_ip;
  struct ip_hdr *ip;
  struct udp_hdr *udp;
  struct netvirt_entry *e;
  void *pkt;
  struct gre_pkt *gre_pkt;

  pkt = rte_pktmbuf_mtod(mb, __u8 *);
  if (ctx->virt_gre)
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
    struct ip_hdr *ip;

    ip = (struct ip_hdr *) (pkt + sizeof(struct eth_hdr));
    outer_remote_ip = f_beui32(ip->dst);
  }

  ret = process_tx_arp(ctx, g, mb, outer_remote_ip);
  if (ret != 0)
    return -1;

  if (ctx->virt_gre)
  {
    process_tx_gre(ctx, g, mb, outer_remote_ip, pkt_len);
  }
  else
  {
    ip = (struct ip_hdr *) (pkt + sizeof(struct eth_hdr));
    udp = (struct udp_hdr *) ((__u8 *) ip + sizeof(struct ip_hdr));

    mb->pkt_len = mb->data_len = pkt_len + sizeof(struct eth_hdr);
    mb->l2_len = sizeof(struct eth_hdr);
    mb->l3_len = sizeof(struct ip_hdr);
    mb->l4_len = sizeof(struct udp_hdr);

    ip = (struct ip_hdr *) (pkt + sizeof(struct eth_hdr));
    mb->pkt_len = mb->data_len = pkt_len + sizeof(struct eth_hdr);
    
    /* TODO: Deal with the offloads in the protocol code */
    if (ip->proto == IP_PROTO_TCP)
    {
      mb->ol_flags = 0;
      mb->ol_flags = RTE_MBUF_F_TX_IPV4 |
          RTE_MBUF_F_TX_IP_CKSUM | RTE_MBUF_F_TX_TCP_CKSUM;
    }
    else
    {
      mb->ol_flags |= RTE_MBUF_F_TX_IPV4 |
          RTE_MBUF_F_TX_IP_CKSUM | RTE_MBUF_F_TX_UDP_CKSUM;
      ip->chksum = 0;
      udp->chksum = rte_ipv4_phdr_cksum((struct rte_ipv4_hdr *) ip,
          mb->ol_flags);
    }
  }

  return 0;
}

static inline int process_rx_arp(struct fast_context *ctx, struct arp_pkt *pkt)
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

static inline void process_tx_gre(struct fast_context *ctx, 
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

  mb->pkt_len = mb->data_len = pkt_len + sizeof(struct eth_hdr) + 
      sizeof(struct ip_hdr) + sizeof(struct gre_hdr);
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

#include <rte_mbuf_core.h>
#include <stdlib.h>
#include <string.h>
#include <linux/types.h>

#include "arp.h"
#include "eth_hdr.h"
#include "ip_hdr.h"
#include "log.h"
#include "queue_types.h"
#include "queue_fns.h"
#include "arp_hdr.h"

static inline __u32 hash_ip(__u32 ip);

void arp_table_init(struct arp_table *arp_table)
{
  int i;
  
  for (i = 0; i < ARP_TABLE_SIZE; i++)
  {
    arp_table->buckets[i].ip = ARP_IP_EMPTY;
    arp_table->buckets[i].pending = 0;
  }
}

struct arp_table_entry * arp_lookup(struct arp_table *at, __u32 ip)
{
  int i;
  __u32 hash, idx; 
  
  hash = hash_ip(ip);

  for (i = 0; i < ARP_TABLE_SIZE; i++) 
  {
    idx = (hash + i) & ARP_TABLE_MASK;
    
    if (at->buckets[idx].ip == ip) 
    {
      return &at->buckets[idx];
    }
    
    if (at->buckets[idx].ip == ARP_IP_EMPTY) 
    {
      return NULL;
    }
  }

  return NULL;
}

struct arp_table_entry * arp_insert(struct arp_table *at, __u32 ip, __u8 *mac)
{
  int i;
  __u32 hash, idx;

  hash = hash_ip(ip);

  for (i = 0; i < ARP_TABLE_SIZE; i++) 
  {
    idx = (hash + i) & ARP_TABLE_MASK;

    if (at->buckets[idx].ip == ARP_IP_EMPTY || at->buckets[idx].ip == ip) 
    {
      at->buckets[idx].ip = ip;
      at->buckets[idx].pending = 0;
      memcpy(at->buckets[idx].mac, mac, 6);
      return &at->buckets[idx];
    }
  }
  
  return NULL;
}

struct arp_table_entry * arp_insert_pending(struct arp_table *at, __u32 ip)
{
  int i;
  __u32 hash, idx;

  hash = hash_ip(ip);

  for (i = 0; i < ARP_TABLE_SIZE; i++) 
  {
    idx = (hash + i) & ARP_TABLE_MASK;

    if (at->buckets[idx].ip == ARP_IP_EMPTY || at->buckets[idx].ip == ip) 
    {
      at->buckets[idx].ip = ip;
      at->buckets[idx].pending = 1;
      return &at->buckets[idx];
    }
  }
  
  return NULL;
}

void arp_buf_init(struct arp_buf *buf)
{
  int i;

  buf->n = 0;
  for (i = 0; i < ARP_BUF_SIZE; i++)
  {
    buf->entries[i].ip = ARP_IP_EMPTY;
    buf->entries[i].mb = NULL;
  }
}

int arp_buf_insert(struct arp_buf *buf, __u32 ip,
    __u16 core, __u8 gid, size_t pkt_len, struct rte_mbuf *mb)
{
  int i;
  struct arp_buf_entry *e;

  for (i = 0; i < ARP_BUF_SIZE; i++)
  {
    e = &buf->entries[i];
    if (e->ip == ARP_IP_EMPTY)
    {
      e->ip = ip;
      e->core = core;
      e->gid = gid;
      e->pkt_len = pkt_len;
      e->mb = mb;
      return 0;
    }
  }

  return -1;
}

int arp_buf_flush(struct arp_buf *buf, __u32 ip, struct equeue **eqs)
{
  int i, ret, flushed;
  struct arp_buf_entry *e;
  struct queue_entry *qe;

  flushed = 0;
  for (i = 0; i < ARP_BUF_SIZE; i++)
  {
    e = &buf->entries[i];
    if (e->ip == ip)
    {
      flushed = 1;
      qe = queue_tail(eqs[e->core]);
      if (qe == NULL)
      {
        LOG_ERROR("control to fast path queue is full");
        return -1;
      }

      qe->data.mbuf_tx.mb = e->mb;
      qe->data.mbuf_tx.gid = e->gid;
      qe->data.mbuf_tx.pkt_len = e->pkt_len;
      ret = queue_enqueue(eqs[e->core], QUEUE_MBUF_TX);
      if (ret < 0)
      {
        LOG_ERROR("failed to enqueue mbuf to fast path");
        return -1;
      }

      e->mb = NULL;
      e->ip = ARP_IP_EMPTY;
    }
  }

  if (flushed)
    return 0;
  else
    return -1;
}

int arp_request(struct equeue *txq, struct equeue *cfq,  
    __u32 remote_ip, __u8 *local_mac, __u32 local_ip)
{
  int ret;
  struct queue_entry *qe;
  struct arp_pkt *pkt;
  __u64 dst_mac = 0xffffffffffffULL;

  qe = queue_tail(txq);
  if (qe == NULL)
  {
    LOG_ERROR("failed to get tail of control txq");
    return -1;
  }
  
  pkt = (void *) &qe->data;
  
  /* Fill ethernet headers */
  memcpy(&pkt->eth.src.addr, local_mac, ETH_ADDR_LEN);
  memcpy(&pkt->eth.dst.addr, &dst_mac, ETH_ADDR_LEN);
  pkt->eth.type = t_beui16(ETH_TYPE_ARP);
  
  /* Fill ARP headers */
  memcpy(&pkt->arp.sha.addr, local_mac, ETH_ADDR_LEN);
  memcpy(&pkt->arp.tha.addr, &dst_mac, ETH_ADDR_LEN);
  pkt->arp.spa = t_beui32(local_ip);
  pkt->arp.tpa = t_beui32(remote_ip);
  pkt->arp.htype = t_beui16(ARP_HTYPE_ETHERNET);
  pkt->arp.ptype = t_beui16(ARP_PTYPE_IPV4);
  pkt->arp.hlen = 6;
  pkt->arp.plen = 4;
  pkt->arp.oper = t_beui16(ARP_OPER_REQUEST);
  
  /* Enqueue packet in transmit queue */
  ret = queue_enqueue(txq, QUEUE_ARP_PKT);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue a packet to transmit queue");
    return -1;
  }
  
  /* Send message to fast signalling that ARP request is ready for transmission */
  ret = queue_enqueue(cfq, QUEUE_ARP_TX_PKT);
  if (ret != 0)
  {
    LOG_ERROR("failed to notify fast-path that ARP request is ready for TX");
    return -1;
  }
  
  return 0;
}

int arp_reply(struct equeue *txq, struct equeue *cfq,
    __u8 *local_mac, __u32 local_ip, __u8 *remote_mac, __u32 remote_ip)
{
  int ret;
  struct queue_entry *qe;
  struct arp_pkt *pkt;

  qe = queue_tail(txq);
  if (qe == NULL)
  {
    LOG_ERROR("failed to get tail of control txq");
    return -1;
  }
  
  pkt = (void *) &qe->data;
  
  /* Fill ethernet headers */
  memcpy(&pkt->eth.src.addr, local_mac, ETH_ADDR_LEN);
  memcpy(&pkt->eth.dst.addr, remote_mac, ETH_ADDR_LEN);
  pkt->eth.type = t_beui16(ETH_TYPE_ARP);
  
  /* Fill ARP headers */
  memcpy(&pkt->arp.sha.addr, local_mac, ETH_ADDR_LEN);
  memcpy(&pkt->arp.tha.addr, remote_mac, ETH_ADDR_LEN);
  pkt->arp.spa = t_beui32(local_ip);
  pkt->arp.tpa = t_beui32(remote_ip);
  pkt->arp.htype = t_beui16(ARP_HTYPE_ETHERNET);
  pkt->arp.ptype = t_beui16(ARP_PTYPE_IPV4);
  pkt->arp.hlen = 6;
  pkt->arp.plen = 4;
  pkt->arp.oper = t_beui16(ARP_OPER_REPLY);
  
  /* Enqueue packet in transmit queue */
  ret = queue_enqueue(txq, QUEUE_ARP_PKT);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue a packet to transmit queue");
    return -1;
  }
  
  /* Send message to fast signalling that ARP request is ready for transmission */
  ret = queue_enqueue(cfq, QUEUE_ARP_TX_PKT);
  if (ret != 0)
  {
    LOG_ERROR("failed to notify fast-path that ARP reply is ready for TX");
    return -1;
  }
  
  return 0;
}

static inline __u32 hash_ip(__u32 ip) 
{
  return ip & ARP_TABLE_MASK;
}

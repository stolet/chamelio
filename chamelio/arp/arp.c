#include <stdlib.h>
#include <string.h>
#include <linux/types.h>

#include "arp.h"
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

struct arp_entry * arp_lookup(struct arp_table *at, __u32 ip) 
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

int arp_insert(struct arp_table *at, __u32 ip, __u8 *mac) 
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
      return 0;
    }
  }
  
  return -1;
}

int arp_insert_pending(struct arp_table *at, __u32 ip) 
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
      return 0;
    }
  }
  
  return -1;
}

int arp_request(struct equeue *txq, struct equeue *cfq,  
    __u32 target_ip, __u8 *src_mac, __u32 src_ip)
{
  int ret;
  struct queue_entry *qe;
  struct pkt_arp *pkt;
  __u64 dst_mac = 0xffffffffffffULL;

  qe = queue_tail(txq);
  if (qe == NULL)
  {
    LOG_ERROR("failed to get tail of control txq");
    return -1;
  }
  
  pkt = (void *) &qe->data;
  
  /* Fill ethernet headers */
  memcpy(&pkt->eth.src.addr, src_mac, ETH_ADDR_LEN);
  memcpy(&pkt->eth.dst.addr, &dst_mac, ETH_ADDR_LEN);
  pkt->eth.type = t_beui16(ETH_TYPE_ARP);
  
  /* Fill ARP headers */
  memcpy(&pkt->arp.sha.addr, src_mac, ETH_ADDR_LEN);
  memcpy(&pkt->arp.tha.addr, &dst_mac, ETH_ADDR_LEN);
  pkt->arp.spa = t_beui32(src_ip);
  pkt->arp.tpa = t_beui32(target_ip);
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
  
  /* Send message to fast that ARP request is ready for transmission */
  ret = queue_enqueue(cfq, QUEUE_ARP_PKT_TX);
  if (ret != 0)
  {
    LOG_ERROR("failed to notify fast-path that ARP packet is ready for TX");
    return -1;
  }
  
  return 0;
}

static inline __u32 hash_ip(__u32 ip) 
{
  return ip & ARP_TABLE_MASK;
}

#include <stdlib.h>
#include <string.h>
#include <linux/types.h>

#include "arp.h"

static inline __u32 hash_ip(__u32 ip);

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
    
    if (at->buckets[idx].ip == 0) 
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

    if (at->buckets[idx].ip == 0 || at->buckets[idx].ip == ip) 
    {
      at->buckets[idx].ip = ip;
      memcpy(at->buckets[idx].mac, mac, 6);
      return 0;
    }
  }
  
  return -1;
}

static inline __u32 hash_ip(__u32 ip) 
{
  return ip & ARP_TABLE_MASK;
}

#ifndef ARP_FAST_H_
#define ARP_FAST_H_

#include "eth_hdr.h"

#define ARP_TABLE_SIZE 64
#define ARP_TABLE_MASK (ARP_TABLE_SIZE - 1)

/* ARP entry in the table */
struct arp_entry {
  /* 1 if this entry was resolved and 0 if it wasn't */
  __u32 ip;
  /* Resolved MAC address */
  __u8 mac[ETH_ADDR_LEN];
};

struct arp_table {
  struct arp_entry buckets[ARP_TABLE_SIZE];
};

struct arp_entry * arp_lookup(struct arp_table *at, __u32 ip);
int arp_insert(struct arp_table *at, __u32 ip, __u8 *mac);

#endif
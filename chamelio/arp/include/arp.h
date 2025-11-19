#ifndef ARP_FAST_H_
#define ARP_FAST_H_

#include "eth_hdr.h"
#include "queue.h"

#define ARP_TABLE_SIZE 64
#define ARP_TABLE_MASK (ARP_TABLE_SIZE - 1)
#define ARP_IP_EMPTY 0

/* ARP entry in the table */
struct arp_entry {
  /* 1 if this entry was resolved and 0 if it wasn't */
  __u32 ip;
  /* 1 if resolution for this address is pending 0 otherwise */
  __u8 pending;
  /* Resolved MAC address */
  __u8 mac[ETH_ADDR_LEN];
};

/* Hash table used to store ARP entries */
struct arp_table {
  struct arp_entry buckets[ARP_TABLE_SIZE];
};

/* Initializes ARP table */
void arp_table_init(struct arp_table *arp_table);
/* Looks up the MAC address for the given IP in the ARp table */
struct arp_entry * arp_lookup(struct arp_table *at, __u32 ip);
/* Maps a MAC address to an IP in the ARP table */
int arp_insert(struct arp_table *at, __u32 ip, __u8 *mac);
/* Inserts an IP to the ARP table and marks it as pending */
int arp_insert_pending(struct arp_table *at, __u32 ip);
/* Sends an ARP request to fast-path for transmission */
int arp_request(struct equeue *txq, struct equeue *cfq,  
    __u32 target_ip, __u8 *src_mac, __u32 src_ip);

#endif
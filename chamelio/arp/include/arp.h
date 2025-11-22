#ifndef ARP_FAST_H_
#define ARP_FAST_H_

#include "eth_hdr.h"
#include "queue.h"
#include "tomgr.h"

/* ARP timeout in ms */
#define ARP_TIMEOUT 10
/* Maximum size of the ARP table */
#define ARP_TABLE_SIZE 64
/* Mask used to get hash for ARP table */
#define ARP_TABLE_MASK (ARP_TABLE_SIZE - 1)
/* Signals that an entry in the table is empty */
#define ARP_IP_EMPTY 0

/* ARP entry in the table */
struct arp_entry {
  /* 1 if this entry was resolved and 0 if it wasn't */
  __u32 ip;
  /* 1 if resolution for this address is pending 0 otherwise */
  __u8 pending;
  /* Resolved MAC address */
  __u8 mac[ETH_ADDR_LEN];
  /* Timeout entry for reply */
  struct to_entry *te;
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
struct arp_entry * arp_insert(struct arp_table *at, __u32 ip, __u8 *mac);
/* Inserts an IP to the ARP table and marks it as pending */
struct arp_entry * arp_insert_pending(struct arp_table *at, __u32 ip);
/* Sends an ARP request to fast-path for transmission */
int arp_request(struct equeue *txq, struct equeue *cfq,  
    __u32 remote_ip, __u8 *local_mac, __u32 local_ip);
/* Sends an ARP reply to fast-path for transmission */
int arp_reply(struct equeue *txq, struct equeue *cfq,
    __u8 *local_mac, __u32 local_ip, __u8 *remote_mac, __u32 remote_ip);

#endif
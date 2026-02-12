#ifndef QUEUE_TYPES_H_
#define QUEUE_TYPES_H_

#include <stddef.h>
#include <linux/types.h>

#include "utils.h"
#include "eth_hdr.h"

/* Type of queue entries */
enum queue_type {
  /* Signals that the queue is empty */
  QUEUE_EMPTY = 0,
  /* Request to register new guest */
  QUEUE_NEW_GUEST_REQ,
  
  /* Request/response for new protocol registration */
  QUEUE_NEW_PROTO_REQ,
  QUEUE_NEW_PROTO_RES,
  
  /* Request/response for creating protocol queues */
  QUEUE_NEW_QUEUE_REQ,
  QUEUE_NEW_QUEUE_RES,
  
  /* Request/Response for creating a protocol map */
  QUEUE_NEW_MAP_REQ,
  QUEUE_NEW_MAP_RES,
  
  /* Request to enable queue in fast-path */
  QUEUE_ENABLEQ_REQ,
  /* Request to disable queue in fast-path */
  QUEUE_DISABLEQ_REQ,

  /* Request/response for allocating memory for eBPF program snippet */
  QUEUE_ALLOCATE_EBPF_REQ,
  QUEUE_ALLOCATE_EBPF_RES,

  /* Request/response for uploading eBPF program snippet */
  QUEUE_UPLOAD_EBPF_REQ,
  QUEUE_UPLOAD_EBPF_RES,

  /* Request/response for freeing eBPF program snippet */
  QUEUE_FREE_EBPF_REQ,
  QUEUE_FREE_EBPF_RES,
  
  /* Message from fast to control signalling failed ARP lookup */
  QUEUE_ARP_LOOKUP,
  /* Message from control to fast so fast-path can update ARP table copy */
  QUEUE_ARP_UPDATE,
  /* Message from fast to control signalling ARP request received */
  QUEUE_ARP_RX_REQ,
  /* Message from fast to control signalling ARP response received */
  QUEUE_ARP_RX_REP,
  /* Message from control to fast signalling ARP request ready for transmission */
  QUEUE_ARP_TX_PKT,
  /* Packet data in transmit queue from control-path */
  QUEUE_ARP_PKT,
};

/* Request for registering new guest */
struct queue_new_guest_req {
  /* Guest ID */
  __u8 id;
  /* Pointer to guest budget counter shared with control */
  __s64 *budget;
  /* Base pointer to shared memory region for this guest */
  void *shm_base;
  /* Length of shared memory region for this guest */
  __u64 shm_len;
  /* GRE key used in network virtualization */
  __u32 gre_key;
  /* Guest IP used in network virtualization */
  __u32 guest_ip;
} __attribute__((packed));

/* Request for registering new protocol */
struct queue_new_proto_req {
  /* Protocol type to register for this context */
  __u8 proto_type; 
} __attribute__((packed));

/* Response for registering new protocol */
struct queue_new_proto_res {
  /* Number of fast-path cores */
  __u32 n_fp_cores;
  /* Local IP address */
  __u32 local_ip;
  /* Size of shm region */
  __u32 shm_len;
} __attribute__((packed));

/* Request to create queues for the protocol */
struct queue_new_queue_req {
  /* Guest ID for this protocol */
  __u8 gid;
  /* Queue ID */
  __u32 qid;
  /* Number of elements in the queue */
  __u32 nelems;
  /* Size of each element in the queue */
  size_t elsize;
  /* Offser to the start of the queue */
  __u64 off;
  /* Pointer to queue struct in library */
  __u64 opaque;
} __attribute__((packed));

/* Response to protocol queue creation */
struct queue_new_queue_res {
  /* ID of the queue */
  __u32 qid;
  /* Number of elements in the queue */
  __u32 nelems;
  /* Size of each element in the queue */
  size_t elsize;
  /* Offset for each queue in shm */
  __u64 off;
  /* Pointer to queue struct in library */
  __u64 opaque;
} __attribute__((packed));

/* Request to create a new protocol map */
struct queue_new_map_req {
  /* Guest ID for this protocol */
  __u8 gid;
  /* Map ID */
  __u32 mid;
  /* Number of elements in each map */
  __u32 nelems;
  /* Size of element in each map */
  __u32 elsize;
  /* Offset to start of map */
  __u64 off;
  /* Pointer to map struct in library */
  __u64 opaque;
} __attribute__((packed));

/* Response to protocol map creation */
struct queue_new_map_res {
  /* Map ID */
  __u16 id;
  /* Offset in shared memory for the map */
  __u64 off;
  /* Number of elements in each map */
  __u32 nelems;
  /* Size of element in each map */
  __u32 elsize;
  /* Pointer to map struct in library */
  __u64 opaque;
} __attribute__((packed));

/* Request to enable queue in fast-path */
struct queue_enableq_req {
  /* Queue ID */
  __u16 qid;
  /* Guest ID */
  __u16 gid;
  /* Queue offset in shared memory */
  __u64 off;
  /* Number of elements in queue */
  __u32 nelems;
  /* Size of elements in queue */
  __u32 elsize;
  /* Fast-path core to enable queue */
  __u16 core;
} __attribute__((packed));

/* Request to disable queue in fast-path */
struct queue_disableq_req {
  /* Queue ID */
  __u16 qid;
  /* Guest ID */
  __u16 gid;
  /* Fast-path core to disable queue */
  __u16 core;
} __attribute__((packed));

/* Request to allocate memory for eBPF program snippet */
struct queue_allocate_ebpf_req {
  /* Size of eBPF program snippet */
  __u32 size;
  /* Pointer to eBPF struct in library */
  __u64 opaque;
} __attribute__((packed));

/* Response to allocate memory for eBPF program snippet */
struct queue_allocate_ebpf_res {
  /* Size of eBPF program snippet allocated */
  __u32 size;
  /* Offset in shared memory for eBPF program snippet */
  __u64 off;
  /* Pointer to eBPF struct in library */
  __u64 opaque;
} __attribute__((packed));

/* Request to upload an EBPF snippet*/
struct queue_up_ebpf_req {
  /* Guest ID */
  __u16 gid;
  /* Size of eBPF program snippet */
  __u32 size;
  /* Offset in shared memory for eBPF program snippet */
  __u64 off;
  /* Function pointer to ebpf rx function. Used only in message to fast */
  struct ebpf_vm_c *event_rx_vm;
  /* Function pointer to ebpf tx function. Used only in message to fast */
  struct ebpf_vm_c *event_tx_vm;
  /* Function pointer to ebpf deq function. Used only in message to fast */
  struct ebpf_vm_c *event_deq_vm;
} __attribute__((packed));

/* Request to free an EBPF snippet */
struct queue_free_ebpf_req {
  /* Guest ID */
  __u16 gid;
  /* Size of eBPF program snippet */
  __u32 size;
  /* Offset in shared memory for eBPF program snippet */
  __u64 off;
} __attribute__((packed));

/* Response sent to slow-path indicating success of ebpf upload */
struct queue_up_ebpf_res {
  /* Indicates if upload was successful: 1 on success 0 on failure */
  int success;
} __attribute__((packed));

/* Response sent to slow-path indicating success of freeing ebpf from SHM */
struct queue_free_ebpf_res {
  /* Indicates if free was successful: 1 on success 0 on failure */
  int success;
} __attribute__((packed));

/* Message sent to control indicating ARP lookup for the given address failed */
struct queue_arp_lookup {
  /* IP for failed ARP lookup */
  __u32 ip;
} __attribute__((packed));

/* Message sent to fast telling it to update ARP table */
struct queue_arp_update {
  /* IP address for ARP mapping */
  __u32 ip;
  /* MAC address for ARP mapping */
  __u8 mac[ETH_ADDR_LEN];
} __attribute__((packed));

/* Message sent to control indicating an ARP request was received */
struct queue_arp_rx_req {
  /* Remote IP */
  __u32 spa;
  /* Local IP */
  __u32 tpa;
  /* Remote MAC address */
  __u64 sha;
} __attribute__((packed));

/* Message sent to control indicating an ARP reply was received */
struct queue_arp_rx_rep {
  /* Remote IP */
  __u32 spa;
  /* Remote MAC address */
  __u64 sha;
  /* Local IP */
  __u32 tpa;
  /* Local MAC address */
  __u64 tha;
} __attribute__((packed));

/* Message sent to fast indicating an ARP request should be transmitted */
struct queue_arp_tx_req {
  /* IP requested in ARP packet */
  __u32 ip;
} __attribute__((packed));


struct queue_entry {
  /* Type of queue entry. Don't update outside of enqueue or dequeue */
  volatile __u8 type;
  /* Data section of queue entry */
  union {
    struct queue_new_guest_req new_guest_req;
    struct queue_new_proto_req new_proto_req;
    struct queue_new_proto_res new_proto_res;
    
    struct queue_new_queue_req new_queue_req;
    struct queue_new_queue_res new_queue_res;
    
    struct queue_new_map_req new_map_req;
    struct queue_new_map_res new_map_res;
    
    struct queue_enableq_req enableq_req;
    struct queue_disableq_req disableq_req;
    
    struct queue_allocate_ebpf_req alloc_ebpf_req;
    struct queue_allocate_ebpf_res alloc_ebpf_res;
    
    struct queue_up_ebpf_req up_ebpf_req;
    struct queue_up_ebpf_res up_ebpf_res;
    
    struct queue_free_ebpf_req free_ebpf_req;
    struct queue_free_ebpf_res free_ebpf_res;
    
    struct queue_arp_lookup arp_lookup;
    struct queue_arp_rx_req arp_pkt_rx_req;
    struct queue_arp_rx_rep arp_pkt_rx_rep;
    struct queue_arp_tx_req arp_pkt_tx_req;
    struct queue_arp_update arp_update;
    
    /* Keeps queue entry the size of a cache line */
    __u8 raw[63];
  } __attribute__((packed)) data;
} __attribute__((packed));

/* We want queue entries to be cache line sized for faster retrieval */
STATIC_ASSERT(sizeof(struct queue_entry) == 64, queue_entry_size);

#endif

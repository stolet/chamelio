#ifndef QUEUE_H_
#define QUEUE_H_

#include <stdlib.h>
#include <stdint.h>

#include "utils.h"
#include "shmalloc.h"

/* Max number of queues a protocol can open */
/* TODO: Don't have this hardcoded */
#define MAX_PROTO_QUEUES 128
#define MAX_PROTO_MAPS 8

/* Type of queue entries */
enum queue_type {
  /* Signals that the queue is empty */
  QUEUE_EMPTY = 0,
  /* Reqiest for new guest registration */
  QUEUE_NEW_GUEST_REQ,
  
  /* Request for new protocol registration */
  QUEUE_PROTO_REQ,
  /* Response for new protocol registration */
  QUEUE_PROTO_RES,
  
  /* Request for creating protocol queues */
  QUEUE_NEW_QUEUE_REQ,
  /* Response from protocol queue creation */
  QUEUE_NEW_QUEUE_RES,
  
  /* Request for creating a protocol map */
  QUEUE_NEW_MAP_REQ,
  /* Response from protocol map creation */
  QUEUE_NEW_MAP_RES,
  
  /* Request to enable queue in fast-path */
  QUEUE_ENABLEQ_REQ,
  /* Request to disable queue in fast-path */
  QUEUE_DISABLEQ_REQ,

  /* Request/Response for allocating memory for eBPF program snippet */
  QUEUE_ALLOCATE_EBPF_REQ,
  QUEUE_ALLOCATE_EBPF_RES,

  /* Request/Response for uploading eBPF program snippet */
  QUEUE_UPLOAD_EBPF_REQ,
  QUEUE_UPLOAD_EBPF_RES,

  /* Request/Response for freeing eBPF program snippet */
  QUEUE_FREE_EBPF_REQ,
  QUEUE_FREE_EBPF_RES
};

/* Request for registering new guest */
struct queue_new_guest_req {
  /* Guest ID */
  uint8_t id;
  /* Base pointer to shared memory region for this guest */
  void *shm_base;
  /* Length of shared memory region for this guest */
  uint64_t shm_len;
} __attribute__((packed));

/* Request for registering new protocol */
struct queue_new_proto_req {
  /* Protocol type to register for this context */
  uint8_t proto_type; 
} __attribute__((packed));

/* Response for registering new protocol */
struct queue_new_proto_res {
  /* Number of fast-path cores */
  uint32_t n_fp_cores;
  /* Size of shm region */
  uint32_t shm_len;
  /* Number of elements in Guest <-> Control queues */
  uint32_t guestq_nelems;
  /* Element size of Guest <-> Control queues */
  uint32_t guestq_elsize;
} __attribute__((packed));

/* Request to create queues for the protocol */
struct queue_new_queue_req {
  /* Guest ID for this protocol */
  uint8_t gid;
  /* Queue ID */
  uint32_t qid;
  /* Number of elements in the queue */
  uint32_t nelems;
  /* Size of each element in the queue */
  size_t elsize;
  /* Offser to the start of the queue */
  uint64_t off;
  /* Pointer to queue struct in library */
  uint64_t opaque;
} __attribute__((packed));

/* Response to protocol queue creation */
struct queue_new_queue_res {
  /* ID of the queue */
  uint32_t qid;
  /* Number of elements in the queue */
  uint32_t nelems;
  /* Size of each element in the queue */
  size_t elsize;
  /* Offset for each queue in shm */
  uint64_t off;
  /* Pointer to queue struct in library */
  uint64_t opaque;
} __attribute__((packed));

/* Request to create a new protocol map */
struct queue_new_map_req {
  /* Guest ID for this protocol */
  uint8_t gid;
  /* Map ID */
  uint32_t mid;
  /* Number of elements in each map */
  uint32_t nelems;
  /* Size of element in each map */
  uint32_t elsize;
  /* Offset to start of map */
  uint64_t off;
  /* Pointer to map struct in library */
  uint64_t opaque;
} __attribute__((packed));

/* Response to protocol map creation */
struct queue_new_map_res {
  /* Map ID */
  uint16_t id;
  /* Offset in shared memory for the map */
  uint64_t off;
  /* Number of elements in each map */
  uint32_t nelems;
  /* Size of element in each map */
  uint32_t elsize;
  /* Pointer to map struct in library */
  uint64_t opaque;
} __attribute__((packed));

/* Request to enable queue in fast-path */
struct queue_enableq_req {
  /* Queue ID */
  uint16_t qid;
  /* Guest ID */
  uint16_t gid;
  /* Queue offset in shared memory */
  uint64_t off;
  /* Number of elements in queue */
  uint32_t nelems;
  /* Size of elements in queue */
  uint32_t elsize;
  /* Fast-path core to enable queue */
  uint16_t core;
} __attribute__((packed));

/* Request to disable queue in fast-path */
struct queue_disableq_req {
  /* Queue ID */
  uint16_t qid;
  /* Guest ID */
  uint16_t gid;
  /* Fast-path core to disable queue */
  uint16_t core;
} __attribute__((packed));

/* Request to allocate memory for eBPF program snippet */
struct queue_allocate_ebpf_req {
  /* Size of eBPF program snippet */
  uint32_t size;
  /* Pointer to eBPF struct in library */
  uint64_t opaque;
} __attribute__((packed));

/* Response to allocate memory for eBPF program snippet */
struct queue_allocate_ebpf_res {
  /* Size of eBPF program snippet allocated */
  uint32_t size;
  /* Offset in shared memory for eBPF program snippet */
  uint64_t off;
  /* Pointer to eBPF struct in library */
  uint64_t opaque;
} __attribute__((packed));

/* Request to free or upload an EBPF snippet*/
struct queue_free_up_ebpf_req {
  /* Size of eBPF program snippet */
  uint32_t size;
  /* Offset in shared memory for eBPF program snippet */
  uint64_t off;
} __attribute__((packed));

struct queue_free_up_ebpf_res {
  /* indicating whether the operation was successful: return 0 if free or upload succesful */
  int success;
} __attribute__((packed));

struct queue_entry {
  /* Type of queue entry. Don't update outside of enqueue or dequeue */
  volatile uint8_t type;
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
    struct queue_free_up_ebpf_req free_up_ebpf_req;
    struct queue_free_up_ebpf_res free_up_ebpf_res;
    /* Keeps queue entry the size of a cache line */
    uint8_t raw[63];
  } __attribute__((packed)) data;
} __attribute__((packed));

/* We want queue entries to be cache line sized for faster retrieval */
STATIC_ASSERT(sizeof(struct queue_entry) == 64, queue_entry_size);

/* This queue is only used for enqueuing */
struct equeue {
  /* Only the side that enqueues updates the tail */
  uint32_t tail;
  /* Number of elements in the queue */
  uint32_t nelems;
  /* Size of each element in the queue */
  size_t elsize;
  /* Offset from the shared memory region to start of the queue */
  uint64_t off;
  /* List of entries that points to the start of the queue in the shm region */
  void *entries;
};

/* This queue is only used for dequeueing */
struct dqueue {
  /* Only the side that dequeues updates the head */
  uint32_t head;
  /* Number of elements in the queue */
  uint32_t nelems;
  /* Size of each element in the queue */
  size_t elsize;
  /* Offset from the shared memory region to start of the queue */
  uint64_t off;
  /* List of entries that points to the start of the queue in the shm region */
  void *entries;
};

/* Allocates a new queue that can only enqueue entries. 
   Prevents race conditions */
struct equeue * equeue_new(uint32_t nelems, size_t elsize, 
    void *addr, uint64_t off);
/* Allocates a new queue that can only dequeue entries. 
   Prevents race conditions */
struct dqueue * dqueue_new(uint32_t nelems, size_t elsize, 
    void *addr, uint64_t off);
/* Initialises the struct for a new equeue */
int equeue_init(struct equeue *q, uint32_t nelems, size_t elsize,
    void *addr, uint64_t off);
/* Initialises the struct for a new dqueue */
int dqueue_init(struct dqueue *q, uint32_t nelems, size_t elsize,
    void *addr, uint64_t off);
/* Advances the tail pointer for the queue */
int queue_enqueue(struct equeue *q, uint8_t type);
/* Advances the head pointer for the queue */
int queue_dequeue(struct dqueue *q);
/* Returns a pointer to the queue entry at the head of the queue */
void * queue_head(struct dqueue *q);
/* Returns a pointer to the next empty queue entry at the tail of the queue */
void * queue_tail(struct equeue *q);

#endif

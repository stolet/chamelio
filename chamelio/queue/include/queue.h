#ifndef QUEUE_H_
#define QUEUE_H_

#include <stdlib.h>
#include <stdint.h>

#include "utils.h"
#include "config.h"
#include "shmalloc.h"

/* Max number of queues a protocol can open */
/* TODO: Don't have this hardcoded */
#define MAX_PROTO_QUEUES 32

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
  QUEUE_NEW_QUEUES_REQ,
  /* Response from protocol queue creation */
  QUEUE_NEW_QUEUES_RES
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
  /* Size of Guest <-> Control queues */
  uint32_t guestq_len;
} __attribute__((packed));

/* Request to create queues for the protocol */
struct queue_new_queues_req {
  /* Gueste ID for this protocol */
  uint8_t gid;
  /* Number of queues to allocate */
  uint16_t nqueues;
  /* Number of elements in each queue */
  uint32_t nelems;
  /* Size of element in each queue */
  uint32_t elsize;
  /* Offset for each queue in shm. Used by the fast-path. */
  uint64_t offs[MAX_PROTO_QUEUES];
};

/* Response to protocol queue creation */
struct queue_new_queues_res {
  /* Number of queues created */
  uint16_t nqueues;
  /* Offset for each queue in shm */
  uint64_t offs[MAX_PROTO_QUEUES];
};

struct queue_entry {
  /* Type of queue entry. Don't update outside of enqueue or dequeue */
  volatile uint8_t type;
  /* Data section of queue entry */
  union {
    struct queue_new_guest_req new_guest_req;
    struct queue_new_proto_req new_proto_req;
    struct queue_new_proto_res new_proto_res;
    struct queue_new_queues_req new_queue_req;
    struct queue_new_queues_res new_queue_res;
    /* Keeps queue entry the size of a cache line */
    uint8_t raw[63];
  } __attribute__((packed)) data;
} __attribute__((packed));

/* We want queue entries to be cache line sized for faster retrieval */
/* TODO: queue_new_queue_res is too big for a cache line size so find
   a workaround */
// STATIC_ASSERT(sizeof(struct queue_entry) == 64, queue_entry_size);

/* This queue is only used for enqueuing */
struct equeue {
  /* Only the side that enqueues updates the tail */
  uint32_t tail;
  /* Size of the queue in bytes */
  uint32_t size;
  /* Offset from the shared memory region to start of the queue */
  uint64_t off;
  /* List of entries that points to the start of the queue in the shm region */
  void *entries;
};

/* This queue is only used for dequeueing */
struct dqueue {
  /* Only the side that dequeues updates the head */
  uint32_t head;
  /* Size of the queue in bytes */
  uint32_t size;
  /* Offset from the shared memory region to start of the queue */
  uint64_t off;
  /* List of entries that points to the start of the queue in the shm region */
  void *entries;
};

/* Creates a new queue that can only enqueue entries. 
   Prevents race conditions */
struct equeue * equeue_new(uint32_t size, void *addr, uint64_t off);
/* Creates a new queue that can only dequeue entries. 
   Prevents race conditions */
struct dqueue * dqueue_new(uint32_t size, void *addr, uint64_t off);
/* Advances the tail pointer for the queue */
int queue_enqueue(struct equeue *q, uint8_t type);
/* Advances the head pointer for the queue */
int queue_dequeue(struct dqueue *q);
/* Returns a pointer to the queue entry at the head of the queue */
struct queue_entry * queue_head(struct dqueue *q);
/* Returns a pointer to the next empty queue entry at the tail of the queue */
struct queue_entry * queue_tail(struct equeue *q);

#endif

#ifndef QUEUE_H_
#define QUEUE_H_

#include <stdlib.h>
#include <stdint.h>

#include "utils.h"
#include "config.h"
#include "shmalloc.h"

/* TODO: Don't have this hardcoded */
#define MAX_FP_CORES 16

/* Type of queue entries */
enum queue_type {
  /* Signals that the queue is empty */
  QUEUE_EMPTY = 0,
  /* Entry for fast path error */
  QUEUE_ERROR,

  /* Entry for ARP in TX */
  QUEUE_ARP_TX,
  /* Entry for ARP in RX */
  QUEUE_ARP_RX,

  /* Entry for new guest registration */
  QUEUE_NEW_GUEST,
  /* Entry for new app registration */
  QUEUE_NEW_APP,
  /* Entry for new app context registration */
  QUEUE_NEW_APP_CTX,
  /* Entry for new app context registration with fast-path */
  QUEUE_NEW_APP_CTX_FAST,

  /* Entry for new Chamelio buffer used for RX or TX */
  QUEUE_NEW_BUF,
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

/* Request for registering new application */
struct queue_new_app_req {
  /* Application ID */
  uint8_t id;
  /* Guest ID */
  uint8_t gid;
  /* Protocol ID */
  uint8_t proto_type;
} __attribute__((packed));

/* Response for registering new application */
struct queue_new_app_res {

} __attribute__((packed));

/* Request for registering new application context */
struct queue_new_app_ctx_req {
  /* Protocol type to register for this context */
  uint8_t proto_type; 
} __attribute__((packed));

/* Request for registering new application context with fast-path */
struct queue_new_app_ctx_fast_req {
  /* Guest ID */
  uint8_t gid;
  /* Application ID */
  uint8_t aid;
  /* Application context ID */
  uint8_t cid;
  /* Protocol type to register for this context */
  uint8_t proto_type; 
  /* Offset in shared memory for rx bump queue for this app context */
  uint64_t rxq_off;
  /* Offser in shared memory for tx bump queue for this app context */
  uint64_t txq_off;
} __attribute__((packed));

/* Response for registering new application context */
struct queue_new_app_ctx_res {
  /* Number of fast-path cores */
  uint32_t n_fp_cores;

  /* Offset in shared memory for Chamelio->App queue */
  uint64_t cham_app_q_off;
  /* Length in shared memory for Chamelio->App queue */
  uint32_t cham_app_q_len;

  /* Offset in shared memory for App->Chamelio queue */
  uint64_t app_cham_q_off;
  /* Length in shared memory for App->Chamelio queue */
  uint32_t app_cham_q_len;

  /* TODO: Can we not hardcode a macro here? */
  /* Length of each RX bump queue */
  uint32_t rx_bump_q_len;
  /* Offset in shared memory for rx queues for each fast-path core */
  uint64_t rx_bump_q_offs[MAX_FP_CORES];
  /* Lengyh of each TX bump queue */
  uint32_t tx_bump_q_len;
  /* Offset in shared memory for tx queues for each fast-path core */
  uint64_t tx_bump_q_offs[MAX_FP_CORES];
} __attribute__((packed));

struct queue_entry {
  /* Type of queue entry. Don't update outside of enqueue or dequeue */
  volatile uint8_t type;
  /* Data section of queue entry */
  union {
    struct queue_new_app_req new_app_req;
    struct queue_new_app_res new_app_res;
    struct queue_new_app_ctx_req new_app_ctx_req;
    struct queue_new_app_ctx_res new_app_ctx_res;
    /* Keeps queue entry the size of a cache line */
    /* TODO: Move this back to a cache line size */
    uint8_t raw[256];
  } __attribute__((packed)) data;
} __attribute__((packed));

/* We want queue entries to be cache line sized for faster retrieval */
/* TODO: Uncomment this */
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

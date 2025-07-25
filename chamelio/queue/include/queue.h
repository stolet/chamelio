#ifndef QUEUE_H_
#define QUEUE_H_

#include <stdlib.h>
#include <stdint.h>

#include "utils.h"
#include "config.h"
#include "shmalloc.h"

/* TODO: Don't have this hardcoded */
#define MAX_FP_CORES 16

enum queue_type {
  QUEUE_EMPTY = 0,
  QUEUE_ERROR,

  QUEUE_ARP_TX,
  QUEUE_ARP_RX,

  QUEUE_NEW_APP,
};

struct queue_new_app_req {
} __attribute__((packed));

struct queue_new_app_res {

} __attribute__((packed));

struct queue_new_app_ctx_req {
  uint32_t rxq_len;
  uint32_t txq_len;
} __attribute__((packed));

struct queue_new_app_ctx_res {
  uint32_t n_fp_cores;

  uint64_t cham_app_q_off;
  uint32_t cham_app_q_len;

  uint64_t app_cham_q_off;
  uint32_t app_cham_q_len;

  uint64_t rxq_offs[MAX_FP_CORES];
  uint64_t txq_offs[MAX_FP_CORES];
} __attribute__((packed));

struct queue_entry {
  volatile uint8_t type;
  union {
    struct queue_new_app_req new_app_req;
    struct queue_new_app_res new_app_res;
    struct queue_new_app_ctx_req new_app_ctx_req;
    struct queue_new_app_ctx_res new_app_ctx_res;
    uint8_t raw[63];
  } __attribute__((packed)) data;
} __attribute__((packed));

/* We want queue entries to be cache line sized for faster retrieval */
/* TODO: Uncomment this */
// STATIC_ASSERT(sizeof(struct queue_entry) == 64, queue_entry_size);

/* Note that this queue is only safe if only one thread enqueues 
   and only one thread dequeues */
struct queue {
  /* TODO: Use pos instead of head and tail because only one side 
     updates this value locally */
  /* Only the side that dequeues updates the head */
  uint32_t head;
  /* Only the side that enqueues updates the tail */
  uint32_t tail;
  /* Size of the queue in bytes */
  uint32_t size;
  /* Handle for shared memory containing the base of this queue */
  struct shm_handle *sh;
  /* List of entries that points to the base of the shm_handle */
  void *entries;
};

struct queue * queue_new(uint32_t size, struct shm_allocator *alloc);
int queue_enqueue(struct queue *q, uint8_t type);
int queue_dequeue(struct queue *q);
struct queue_entry * queue_head(struct queue *q);

#endif

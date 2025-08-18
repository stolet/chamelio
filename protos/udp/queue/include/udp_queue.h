#ifndef UDP_QUEUE_H_
#define UDP_QUEUE_H_

#include <stdlib.h>
#include <stdint.h>

#include "shmalloc.h"
#include "queue.h"

/* Type of queue entries */
enum udp_queue_type {
  /* Signals that the queue is empty */
  UDP_QUEUE_EMPTY = 0,
  /* Request to slow-path to register new app */
  UDP_QUEUE_NEW_ACTX_REQ,
  /* Response from slow-path after registering app */
  UDP_QUEUE_NEW_ACTX_RES,
};

/* Request for app context to register with slow-path */
struct udp_queue_new_actx_req {
  /* Add a byte value to request so it's not empty */
  uint8_t req;
} __attribute__((packed));

/* Response when app context registers with slow-path */
struct udp_queue_new_actx_res {
  /* Number of fast-path cores */
  uint32_t n_fp_cores;
  /* Size of shm region */
  uint32_t shm_len;
  /* Size of app->slow queue */
  uint32_t as_len;
  /* Offset in shm of app->slow queue */
  uint64_t as_off;
  /* Size of slow->app queue */
  uint32_t sa_len;
  /* Offset in shm of slow->app queue */
  uint64_t sa_off;
} __attribute__((packed));

struct udp_queue_entry {
  /* Type of queue entry. Don't update outside of enqueue or dequeue */
  volatile uint8_t type;
  /* Data section of queue entry */
  union {
    struct udp_queue_new_actx_req new_actx_req;
    struct udp_queue_new_actx_res new_actx_res;
    /* Keeps queue entry the size of a cache line */
    uint8_t raw[63];
  } __attribute__((packed)) data;
} __attribute__((packed));

/* We want queue entries to be cache line sized for faster retrieval */
STATIC_ASSERT(sizeof(struct udp_queue_entry) == 64, udp_queue_entry_size);

/* Advances the tail pointer for the queue */
int udp_queue_enqueue(struct equeue *q, uint8_t type);
/* Advances the head pointer for the queue */
int udp_queue_dequeue(struct dqueue *q);
/* Returns a pointer to the queue entry at the head of the queue */
struct udp_queue_entry * udp_queue_head(struct dqueue *q);
/* Returns a pointer to the next empty queue entry at the tail of the queue */
struct udp_queue_entry * udp_queue_tail(struct equeue *q);

#endif

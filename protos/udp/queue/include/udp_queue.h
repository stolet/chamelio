#ifndef UDP_QUEUE_H_
#define UDP_QUEUE_H_

#include <stdlib.h>
#include <stdint.h>

#include "shmalloc.h"
#include "queue.h"

#define MAX_FP_CORES 16

/* Type of queue entries */
enum udp_queue_type {
  /* Signals that the queue is empty */
  UDP_QUEUE_EMPTY = 0,
  /* Request to slow-path to register new app */
  UDP_QUEUE_NEW_ACTX_REQ,
  /* Response from slow-path after registering app */
  UDP_QUEUE_NEW_ACTX_RES,
  /* Request to slow-path to create new socket */
  UDP_QUEUE_NEW_SOCK_REQ,
  /* Response from slow-path for created socket */
  UDP_QUEUE_NEW_SOCK_RES,
  /* Bump message to update buffer available or head */
  UDP_QUEUE_BUMP,
};

/* Request to create new socket in slow-path */
struct udp_queue_new_sock_req {
  /* Pointer to socket in the library */
  uint64_t opaque;
} __attribute__((packed)); 

/* Response for new socket created */
struct udp_queue_new_sock_res {
  /* Pointer to socket in the library */
  uint64_t opaque;
  /* ID of the socket in slow-path */
  uint32_t id_slow;
  /* Queue ID of RX buffer */
  uint16_t rx_qid;
  /* Length of RX buffer */
  uint32_t rx_len;
  /* Offset of RX buffer */
  uint64_t rx_off;
  /* Queue ID of TX buffer */
  uint16_t tx_qid;
  /* Length of TX buffer */
  uint32_t tx_len;
  /* Offset of TX buffer */
  uint64_t tx_off;
} __attribute__((packed));

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
  /* Size of app->fast queues */
  uint32_t af_len;
  /* Offsets for app->fast bump queues */
  uint64_t af_offs[MAX_FP_CORES];
  /* Size of fast->app queues */
  uint32_t fa_len;
  /* Offsets for fast->app bump queues */
  uint64_t fa_offs[MAX_FP_CORES];
} __attribute__((packed));

/* Message that bumps the RX and TX avail or head */
struct udp_queue_bump {
  /* Bump for RX available */
  uint32_t rx_avail;
  /* Bump for RX head */
  uint32_t rx_head;
  /* Bump for TX available */
  uint32_t tx_avail;
  /* Bump for TX head */
  uint32_t tx_head;
} __attribute__((packed));

struct udp_queue_entry {
  /* Type of queue entry. Don't update outside of enqueue or dequeue */
  volatile uint8_t type;
  /* Data section of queue entry */
  union {
    struct udp_queue_new_actx_req new_actx_req;
    struct udp_queue_new_actx_res new_actx_res;
    struct udp_queue_new_sock_req new_sock_req;
    struct udp_queue_new_sock_res new_sock_res;
    struct udp_queue_bump bump;
    /* Keeps queue entry the size of a cache line */
    uint8_t raw[511];
  } __attribute__((packed)) data;
} __attribute__((packed));

/* TODO: Make this cache-lined again. The udp_queue_new_actx_res
   probably doesn't have to be in udp_queue.h */
/* We want queue entries to be cache line sized for faster retrieval */
STATIC_ASSERT(sizeof(struct udp_queue_entry) == 512, udp_queue_entry_size);

/* Advances the tail pointer for the queue */
int udp_queue_enqueue(struct equeue *q, uint8_t type);
/* Advances the head pointer for the queue */
int udp_queue_dequeue(struct dqueue *q);
/* Returns a pointer to the queue entry at the head of the queue */
struct udp_queue_entry * udp_queue_head(struct dqueue *q);
/* Returns a pointer to the next empty queue entry at the tail of the queue */
struct udp_queue_entry * udp_queue_tail(struct equeue *q);

#endif

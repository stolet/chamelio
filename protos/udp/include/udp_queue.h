#ifndef UDP_QUEUE_H_
#define UDP_QUEUE_H_

#include <stdlib.h>
#include <stdint.h>
#include <cham_fast.h>

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
  /* Request to slow-path to create new socket */
  UDP_QUEUE_NEW_SOCK_REQ,
  /* Response from slow-path for created socket */
  UDP_QUEUE_NEW_SOCK_RES,
  /* Bind message for a socket */
  UDP_QUEUE_BIND,
  /* Bumps Chamelio when app calls send */
  UDP_QUEUE_BUMP_CHAM_TX,
  /* Bump Chamelio when app calls recv */
  UDP_QUEUE_BUMP_CHAM_RX,
  /* Bump app when Chamelio sends a packet */
  UDP_QUEUE_BUMP_APP_TX,
  /* Bump app when Chamelio receives a packet */
  UDP_QUEUE_BUMP_APP_RX,
};

/* Request to create new socket in slow-path */
struct udp_queue_new_sock_req {
  /* Pointer to socket in the library */
  uint64_t opaque;
} __attribute__((packed)); 

/* Response for new socket created */
struct udp_queue_new_sock_res {
  /* ID of the socket in slow-path */
  uint32_t sock_id;
  /* Pointer to socket in the library */
  uint64_t opaque;
  /* Fast-path core this socket is running */
  uint16_t core;
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
  /* NUmber of elements app->slow queue */
  uint32_t as_nelems;
  /* Size of elements app->slow queue */
  uint32_t as_elsize;
  /* Offset in shm of app->slow queue */
  uint64_t as_off;
  /* Number of elements slow->app queue */
  uint32_t sa_nelems;
  /* Size of elements slow->app queue */
  uint32_t sa_elsize;
  /* Offset in shm of slow->app queue */
  uint64_t sa_off;
  /* Number of elements app->fast queues */
  uint32_t af_nelems;
  /* Size of elements app->fast queues */;
  uint32_t af_elsize;
  /* Offsets for app->fast bump queues */
  uint64_t af_offs[MAX_FP_CORES];
  /* Number of elements fast->app queues */
  uint32_t fa_nelems;
  /* Size of elements fast->app queues */
  uint32_t fa_elsize;
  /* Offsets for fast->app bump queues */
  uint64_t fa_offs[MAX_FP_CORES];
} __attribute__((packed));

/* Message that sends the src port and ip */
struct udp_queue_bind {
  /* Socket ID used by slow-path */
  uint32_t sock_id;
  /* Source port for this bind */
  uint16_t src_port;
  /* Source IP for this bind */
  uint32_t src_ip;
} __attribute__((packed));

/* Message that bumps the Chamelio TX avail */
struct udp_queue_bump_cham_tx {
  /* Socket ID used by slow-path */
  uint32_t sock_id;
  /* TX port for this bump */
  uint16_t tx_port;
  /* TX IP for this bump */
  uint32_t tx_ip;
  /* Bump for TX available */
  uint32_t tx_avail;
} __attribute__((packed));

/* Message that bumps the Chamelio RX head */
struct udp_queue_bump_cham_rx {
  /* Socket ID used by slow-path */
  uint32_t sock_id;
  /* Bump for RX head */
  uint32_t rx_head;
} __attribute__((packed));

/* Message that bumps the app TX head */
struct udp_queue_bump_app_tx {
  /* Opaque pointer to struct in app library */
  uint64_t opaque;
  /* Bump for TX head */
  uint32_t tx_head;
} __attribute__((packed));

/* Message that bumps the app RX available */
struct udp_queue_bump_app_rx {
  /* Opaque pointer to struct in app library */
  uint64_t opaque;
  /* RX port for this bump */
  uint16_t rx_port;
  /* RX IP for this bump */
  uint32_t rx_ip;
  /* Bump for RX available */
  uint32_t rx_avail;
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
    struct udp_queue_bind bind;
    /* Keeps queue entry the size of half a cache line */
    uint8_t raw[511];
  } __attribute__((packed)) data;
} __attribute__((packed));

struct udp_queue_bump_entry {
  /* Type of queue entry. Don't upadete outside of enqueue or dequeue */
  volatile uint8_t type;
  /* Data section of queue entry */
  union {
    struct udp_queue_bump_app_tx bump_app_tx;
    struct udp_queue_bump_app_rx bump_app_rx;
    struct udp_queue_bump_cham_tx bump_cham_tx;
    struct udp_queue_bump_cham_rx bump_cham_rx;
    /* Keeps queue entry the size of a cache line */
    uint8_t raw[31];
  } __attribute__((packed)) data;
} __attribute__((packed));

/* TODO: Make this cache-aligned again. The udp_queue_new_actx_res
   probably doesn't have to be in udp_queue.h */
/* We want queue entries to be cache line sized for faster retrieval */
STATIC_ASSERT(sizeof(struct udp_queue_entry) == 512, udp_queue_entry_size);
STATIC_ASSERT(sizeof(struct udp_queue_bump_entry) == 32, udp_bump_queue_entry_size);

#endif

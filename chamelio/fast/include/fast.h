#ifndef FAST_H_
#define FAST_H_

#include <stdint.h>

#include <rte_ethdev.h>

#include "cham_fast.h"
#include "nic.h"
#include "nic_fast.h" 
#include "config.h"
#include "queue.h"

#define BATCH_SIZE 16

#define PROTOQ_DISABLED 0
#define PROTOQ_ENABLED 1

/* We want the TXBUF_SIZE to be double the BATCH_SIZE so we can 
   fit packets from the TX phase and ACKs sent in the receive phase */
#define TXBUF_SIZE 2 * BATCH_SIZE

/* Types of protocols supported by Chamelio */
enum protocol_type {
  PROTO_UDP = 0,
  PROTO_TCP,
  PROTO_RDMA,
};

struct proto_fast {
  /* Number of enabled queues */
  uint16_t ndqueues;
  /* Head of enabled queues */
  uint16_t dqueues_head;
  /* Tail of enabled queues */
  uint16_t dqueues_tail;
  /* Array of nodes for enabled queues dequeued by Chamelio */
  struct cham_dqueue dqueues[MAX_PROTO_QUEUES];
  /* Handle containing protocol state passed to custom fast-path */  
  struct cham_proto_handle handle;

  /* Processes one received packet */
  int (*event_rx)(void *pkt);
  /* Processes one scheduled packet for transmissiojn */
  int (*event_tx)(void *pkt, struct cham_proto_handle *handle);
  /* Dequeue and process entry from a queue */
  int (*event_deq)(int qid, struct queue_entry *qe, 
      struct cham_proto_handle *handle);
};

struct guest_fast {
  /* Guest ID */
  uint8_t id;
  /* Protocol to use with this guest */
  struct proto_fast proto;
  /* Base pointer to shared memory region for this guest */
  void *shm_base;
  /* Length of shared memory region for this guest */
  uint64_t shm_len;
};

struct fast_context {
  /* ID of this fast-path context */
  uint8_t id;
  /* NIC context for the fast-path */
  struct nic_fast_context nic_ctx;
  
  /* Number of guests registered so far in this fast-path */
  uint8_t n_guests;
  /* List of guests registered so far in this fast-path */
  struct guest_fast *guests;

  /* Number of mbufs that are processed and ready to be transmitted */
  uint16_t tx_n;
  /* List of mbuf pointers that are processed and ready to be transmitted */
  struct rte_mbuf *tx_mbs[TXBUF_SIZE];

  /* Queue from fast-path core to control-path */
  struct equeue *fast_ctl_q;
  /* Queue from the control-path to the fast-path */
  struct dqueue *ctl_fast_q;

  /* File descriptor for internal shared memory */
  int shm_fd_internal;
  /* Base pointer for internal shared memory region */
  void *shm_base_internal;
};

/* Initialises the fast-path context when a core is launched */
int fast_context_init(struct fast_context *f_ctx, 
    struct nic_context *nic_ctx, uint16_t thread_id,
    struct shm_handle *fc_handle, struct shm_handle *cf_handle,
    struct configuration *config, int shm_fd_internal, void *shm_base_internal);
/* Dataplane loop in a fast-path core */
int fast_loop(struct fast_context *ctx);
/* Cleans up the fast-path */
void fast_context_destroy();

#endif
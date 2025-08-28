#ifndef FAST_H_
#define FAST_H_

#include <stdint.h>

#include <rte_ethdev.h>

#include "cham_fast.h"
#include "nic.h"
#include "nic_fast.h" 
#include "config.h"
#include "queue.h"
#include "scheduler.h"

#define BATCH_SIZE 16

#define PROTOQ_DISABLED 0
#define PROTOQ_ENABLED 1
#define PROTOQ_ID_INVALID UINT16_MAX

/* We want the TXBUF_SIZE to be double the BATCH_SIZE so we can 
   fit packets from the TX phase and ACKs sent in the receive phase */
#define TXBUF_SIZE 2 * BATCH_SIZE

/* Types of protocols supported by Chamelio */
enum protocol_type {
  PROTO_UDP = 0,
  PROTO_TCP,
  PROTO_RDMA,
};

struct proto_queue_fast {
  /* ID of this protocol queue */
  uint16_t id;
  /* Queue structure */
  struct dqueue dq;
  /* Next queue in active list */
  uint16_t next;
  /* Previous queue in list */
  uint16_t prev;
  /* Protocol this queue belongs to */
  struct proto_fast *proto;
};

struct proto_map_fast {
  /* ID of this map in protocol */
  uint16_t id;
  /* Number of elements in the map */
  uint32_t nelems;
  /* Size of each element in the map */
  uint32_t elsize;
  /* Offset in shared memory where this map starts */
  uint64_t off;
  /* Protocol this map belongs to */
  struct proto_fast *proto;
};

struct proto_fast {
  /* Number of queues */
  uint16_t nqueues;
  /* Head of enabled queues */
  uint16_t queues_head;
  /* Tail of enabled queues */
  uint16_t queues_tail;
  /* List of enabled queues queues in shared memory */
  struct proto_queue_fast queues[MAX_PROTO_QUEUES];

  /* Number of maps in shared memory */
  uint16_t nmaps;
  /* List of maps in shared memory */
  struct proto_map_fast maps[MAX_PROTO_MAPS];

  /* TX scheduler for this protocol */
  struct scheduler sched;

  /* TODO: Pass protocol handler */
  /* Initialises the fast-path */
  int (*init_fp)(void *config);
  /* Processes one received packet */
  int (*event_rx)(void *pkt);
  /* Processes one scheduled packet for transmissiojn */
  int (*event_tx)(void *pkt, struct cham_ready_entry *re);
  /* Dequeue and process entry from a queue */
  int (*event_deq)(int qid, struct queue_entry *qe, struct cham_sched_entry *se);
  /* Schedules packet for transmission */
  int (*act_txsched)(struct cham_sched_entry *se, struct cham_ready_entry *re);
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

  /* TODO: Pass internal fd and base to fast-path */
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
/* Cleansup the fast-path */
void fast_context_destroy();

#endif
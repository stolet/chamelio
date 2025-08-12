#ifndef FAST_H_
#define FAST_H_

#include <stdint.h>

#include <rte_ethdev.h>

#include "nic.h"
#include "nic_fast.h" 
#include "config.h"
#include "queue.h"

#define BATCH_SIZE 16

/* Types of protocols supported by Chamelio */
enum protocol_type {
  PROTO_UDP = 0,
  PROTO_TCP,
  PROTO_RDMA,
};

/* We want the TXBUF_SIZE to be double the BATCH_SIZE so we can 
   fit packets from the TX phase and ACKs sent in the receive phase */
#define TXBUF_SIZE 2 * BATCH_SIZE

struct protocol {
  /* Protocol ID */
  enum protocol_type type;
  /* Processes one received packet */
  uint8_t (*process_rx)(void *, struct rte_mbuf *);
  /* Processes one packet to transmit */
  uint8_t (*process_tx)(void *, struct rte_mbuf *);
  /* Polls application queues to check how much data to transmit */
  uint8_t (*process_queues)();
};

struct proto_fast {

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

  /* TODO: Pass internnal fd and base to fast-path */
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
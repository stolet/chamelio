#ifndef FAST_H_
#define FAST_H_

#include <stdint.h>

#include <rte_ethdev.h>

#include "nic.h"
#include "nic_fast.h" 
#include "config.h"
#include "queue.h"

#define BATCH_SIZE 16

/* We want the TXBUF_SIZE to be double the BATCH_SIZE so we can 
   fit packets from the TX phase and ACKs sent in the receive phase */
#define TXBUF_SIZE 2 * BATCH_SIZE

#define PROTOCOL_UDP 1
#define PROTOCOL_TCP 2
#define PROTOCOL_RDMA 3

struct protocol {
  uint8_t id;

  uint64_t shm_len;
  void * shm;
  uint64_t shm_internal_len;
  void *shm_internal;

  uint8_t (*process_rx)(void *, struct rte_mbuf *);
  uint8_t (*process_tx)(void *, struct rte_mbuf *);
  uint8_t (*process_queues)();
};

struct application_fast {
  uint8_t id;
  struct protocol proto;
};

struct guest_fast {
  uint8_t id;
  struct guest_fast *next_guest;

  uint8_t n_apps;
  struct application_fast *apps;

  void *shm_base;
  uint64_t shm_len;
  void *shm_internal_base;
  uint64_t shm_internal_len;
};

struct fast_context {
  uint8_t id;
  struct nic_fast_context nic_ctx;
  
  uint8_t n_guests;
  struct guest_fast *guests;

  uint16_t tx_n;
  struct rte_mbuf *tx_mbs[TXBUF_SIZE];

  /* Queue from fast-path core to slow-path */
  struct equeue *fast_slow_q;
  /* Queue from the slow-path to the fast-path */
  struct dqueue *slow_fast_q;
};

int fast_context_init(struct fast_context *f_ctx, 
    struct nic_context *nic_ctx, uint16_t thread_id,
    struct shm_handle *fs_handle, struct shm_handle *sf_handle,
    struct configuration *config);
int fast_loop(struct fast_context *ctx);
void fast_context_destroy();

#endif
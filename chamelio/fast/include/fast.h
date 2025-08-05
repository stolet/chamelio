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

struct app_context_fast {
  /* Application context ID */
  uint8_t id;
  /* Pointer to the application for this context */
  struct app_fast *app;
  /* List of app bump queue offset */
  uint64_t app_bump_q_off;
  /* List of tx queue offset for each app context and fast-path core */
  uint64_t cham_bump_q_off;
};

struct app_fast {
  /* Application ID */
  uint8_t id;
  /* Pointer to guest where this application is running */
  struct guest_fast *guest;
  /* Protocol registered for this application */
  struct protocol proto;
  /* Number of application contexts */
  uint8_t n_app_ctxs;
  /* List of application contexts */
  struct app_context_fast *app_ctxs;
  /* Table with registered send or tx Chamelio buffers */
  struct cham_buf_ht *buf_ht;
};

struct guest_fast {
  /* Guest ID */
  uint8_t id;
  /* Number of apps registered in this guest */
  uint8_t n_apps;
  /* List of apps registered in this guest */
  struct app_fast *apps;
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

  /* Queue from fast-path core to slow-path */
  struct equeue *fast_slow_q;
  /* Queue from the slow-path to the fast-path */
  struct dqueue *slow_fast_q;

  /* TODO: Pass internnal fd and base to fast-path */
  /* File descriptor for internal shared memory */
  int shm_fd_internal;
  /* Base pointer for internal shared memory region */
  void *shm_base_internal;
};

/* Initialises the fast-path context when a core is launched */
int fast_context_init(struct fast_context *f_ctx, 
    struct nic_context *nic_ctx, uint16_t thread_id,
    struct shm_handle *fs_handle, struct shm_handle *sf_handle,
    struct configuration *config, int shm_fd_internal, void *shm_base_internal);
/* Dataplane loop in a fast-path core */
int fast_loop(struct fast_context *ctx);
/* Cleansup the fast-path */
void fast_context_destroy();

#endif
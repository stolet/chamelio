#ifndef CHAM_LIB_H_
#define CHAM_LIB_H_

#include <stdint.h>

#include "utils.h"

// TODO: Don't duplicate this
#define GUEST_SOCKET_PATH "guest_socket"
#define APP_SOCKET_PATH "app_socket"
#define IVSHMEM_PROTOCOL_VERSION 0
#define HOST_PEERID 255
#define MAX_FP_CORES 16

/* Chamelio buffers that can be used for RX or TX */
struct buff_lib {
  /* Base address of buffer */
  uint64_t base;
  /* Length of the buffer */
  uint32_t len;
  /* Head of the queue */
  uint64_t head;
  /* Number of bytes available */
  uint32_t avail;
};

/* TODO: Get number of fp-cores for bump queues from 
   queue_new_app_ctx_res instead of hardcoded macro. */
/* Library representation of Chamelio application context */
struct app_context_lib {
  /* Application context id */
  uint8_t id;
  /* Number of fast-path cores */
  uint32_t n_fp_cores;
  /* Application */
  struct app_lib *app;
  /* Queue from the application context to the Chamelio slow-path */
  struct equeue *app_cham_q;
  /* Queue from the Chamelio slow-path to the application context */
  struct dqueue *cham_app_q;
  /* Queues for RX bumps. One per FP core. */
  struct dqueue *rx_bump_q[MAX_FP_CORES];
  /* Queues for TX bumps. One per FP core. */
  struct equeue *tx_bump_q[MAX_FP_CORES];
  /* Next context in list */
  struct app_context_lib *next;
};

/* Library representation of Chamelio application */
struct app_lib {
  /* Unix socket used to register with Chamelio */
  int uxsocket_fd;
  /* File descriptor for shared memory region used by this app */
  int shm_fd;
  /* Base pointer for shared memory region used by this app */
  void *shm_base;
  /* Number of application contexts running */
  int n_ctxs;
  /* List of application contexts */
  struct app_context_lib *ctxs;
};

/* Initialises a guest with Chamelio */
int cham_init_guest();
/* Initialises a new application with Chamelio */
struct app_lib * cham_init_app();
/* Initialises a new application context with Chamelio*/
struct app_context_lib * cham_init_app_ctx(struct app_lib *a, uint8_t proto_type);

/* Polls for messages from Chamelio slow-path */
int poll_chamelio(struct app_context_lib *actx);
/* Polls for bump messages from rx */
int poll_bump(struct app_context_lib *actx);

#endif
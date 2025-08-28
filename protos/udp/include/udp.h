#ifndef UDP_H_
#define UDP_H_

#include <stdint.h>

#include "queue.h"

#define UDP_MSS 1400

#define MAX_OFFS 8
#define MAX_APPS 8
#define MAX_CTXS 8
#define MAX_READY 16
#define MAX_SOCKETS 8192
#define MAX_SCHED MAX_SOCKETS

#define ID_INVALID (-1U)

/* Entry for the socket map */
struct udp_sock {
  /* Socket ID */
  uint32_t id;
  /* Pointer to socket in application */
  uint64_t opaque;
  /* ID of next socket in list */
  uint32_t next_id;
  /* Fast-path core this socket is currently running on */
  uint16_t core;
  /* Queue ID to bump app */
  uint16_t app_bump_qid;

  /* Queue ID used for RX buffer */
  uint16_t rx_qid;
  /* Length of RX buffer */
  uint32_t rx_len;
  /* Number of available bytes to be read */
  uint32_t rx_avail;
  /* Head of RX buffer */
  uint32_t rx_head;
  /* Pointer to start of RX buffer in shared memory */
  void *rx_buf;
  
  /* Queue ID used for TX buffer */
  uint16_t tx_qid;
  /* Length of the TX buffer */
  uint32_t tx_len;
  /* Number of bytes written to buffer */
  uint32_t tx_avail;
  /* Head of the TX buffer */
  uint32_t tx_head;
  /* Pointer to the start of the TX buffer in shared memory */
  void *tx_buf;
};

/* Entry for app bump map */ 
struct udp_app_bump_mape {
  /* Queue ID */
  uint16_t id;
  /* Next ID */
  uint16_t next_id;
  /* Queue only for enqueueing */
  struct equeue q;  
};

#endif
#ifndef UDP_H_
#define UDP_H_

#include <stdint.h>

#define UDP_MSS 1400

#define MAX_OFFS 2
#define MAX_APPS 8
#define MAX_CTXS 8
#define MAX_READY 16
#define MAX_SOCKETS 8192
#define MAX_SCHED MAX_SOCKETS

#define ID_INVALID (-1U)

/* Types of possible maps in UDP protocol */
enum udp_map_type {
  MTYPE_SOCKS = 0,
  MTYPE_TXSCHED,
  MTYPE_TXREADY,
};

/* Entry for the offset map */
struct udp_off_mape
{
  /* Identifying ID of what this entry is for */
  enum udp_map_type id;
  /* Offset in shared memory for this other map */
  uint64_t off;
  /* Number of elements actually added to this map */
  uint32_t n;
  /* Max number of elements in this map */
  uint32_t max_n;
  /* Key for first entry in the map */
  uint32_t head;
  /* Key for last entry in the map */
  uint32_t tail;
};

/* Entry for the socket map */
struct udp_sock_mape {
  /* Socket ID */
  uint32_t id;
  /* ID of next socket in list */
  uint32_t next_id;

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

/* Entry for TX scheduler map */
struct udp_txsched_mape {
  /* Socket ID */
  uint32_t id;
  /* ID of next socket in schedler queue */
  uint32_t next_id;
  /* Number of bytes available to be scheduled */
  uint32_t tx_avail;
};

/* Entry for TX ready map */
struct udp_txready_mape {
  /* TX ready ID */
  uint32_t id;
  /* ID of next ready in list */
  uint32_t next_id;
  /* Socket ID */
  uint32_t sock_id;
  /* Amount of bytes to be transmitted */
  uint32_t tx_ready;
};

#endif
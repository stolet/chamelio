#ifndef UDP_H_
#define UDP_H_

#include <linux/types.h>

#define UDP_MSS 1400

#define MAX_OFFS 8
#define MAX_APPS 8
#define MAX_CTXS 8
#define MAX_READY 16
#define MAX_SOCKETS 8192
#define MAX_SCHED MAX_SOCKETS

#define ID_INVALID (-1U)

#define UDP_EBPF_BYTECODE "protos/udp/fast/udp_fast.bpf.o"

/* Entry for the socket map */
struct udp_sock {
  /* Socket ID */
  __u32 id;
  /* Pointer to socket in application */
  __u64 opaque;
  /* ID of next socket in list */
  __u32 next_id;
  /* Fast-path core this socket is currently running on */
  __u16 core;
  /* Queue ID to bump app */
  __u16 app_bump_qid;
  /* Destination IP */
  __u32 dst_ip;
  /* Destination port */
  __u16 dst_port;
  /* Source IP */
  __u32 src_ip;
  /* Source port */
  __u16 src_port;

  /* Queue ID used for RX buffer */
  __u16 rx_qid;
  /* Length of RX buffer */
  __u32 rx_len;
  /* Number of available bytes to be read */
  __u32 rx_avail;
  /* Head of RX buffer */
  __u32 rx_head;
  /* Pointer to start of RX buffer in shared memory */
  __u64 rx_off;
  
  /* Queue ID used for TX buffer */
  __u16 tx_qid;
  /* Length of the TX buffer */
  __u32 tx_len;
  /* Number of bytes written to buffer */
  __u32 tx_avail;
  /* Head of the TX buffer */
  __u32 tx_head;
  /* Offset to the start of the TX buffer in shared memory */
  __u64 tx_off;
};

/* Entry for app bump map */ 
struct udp_app_bump_mape {
  /* Queue ID */
  __u16 id;
  /* Next ID */
  __u16 next_id;
  /* Queue only for enqueueing */
  struct equeue q;  
};

#endif
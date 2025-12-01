#ifndef UDP_H_
#define UDP_H_

#include <linux/types.h>

/* Maximum segment size for UDP */
#define UDP_MSS 1400
/* TODO: Pass MAX_APPS and MAX_CTXS as a parameter to UDP slow-path */
/* Max number of applications that can register with slow-path */
#define MAX_APPS 8
/* Max number of contexts per application */
#define MAX_CTXS 8
/* Minumum port number */
#define MIN_PORT 1002
/* Maximum port number */
#define MAX_PORT 65534
/* Maximum number of UDP sockets */
#define MAX_SOCKETS MAX_PORT
/* Max number of reusable ports */
#define MAX_REUPORTS MAX_PORT
/* Max number of sockets that can bind to reusable port */
#define MAX_REUSOCK_PORT MAX_CTXS
STATIC_ASSERT(MAX_REUSOCK_PORT % 2 == 0, max_reusock_port);
/* Max number of scheduler entries */
#define MAX_SCHED MAX_SOCKETS
/* Used to signal that a socket entry is invalid/empty */
#define ID_INVALID (-1U)
/* Location where ebpf bytecode is located */
#define UDP_EBPF_BYTECODE "protos/udp/fast/udp_fast.bpf.o"

/* Entry for the socket map */
struct udp_sock {
  /* Socket ID */
  __u32 id;
  /* Pointer to socket in application */
  __u64 opaque;
  /* Fast-path core this socket is currently running on */
  __u16 core;
  /* Queue ID to bump app */
  __u16 app_bump_qid;
  /* Local IP */
  __u32 local_ip;
  /* Local port */
  __u16 local_port;
  /* 1 if this socket reuses a port 0 otherwise */
  __u8 reuport;

  /* Length of RX buffer */
  __u32 rx_len;
  /* Number of available bytes to be read */
  __u32 rx_avail;
  /* Head of RX buffer */
  __u32 rx_head;
  /* Pointer to start of RX buffer in shared memory */
  __u64 rx_off;
  
  /* Length of the TX buffer */
  __u32 tx_len;
  /* Number of bytes written to buffer */
  __u32 tx_avail;
  /* Head of the TX buffer */
  __u32 tx_head;
  /* Offset to the start of the TX buffer in shared memory */
  __u64 tx_off;
} __attribute__((packed));

/* Maps a port to a socket id */
struct udp_port {
  /* Number of sockets bounded to this port */
  __u32 nsocks;
  /* Next socket to load balance to if a message is received */
  __u32 next_sock;
  /* Socket IDs */
  __u32 sids[MAX_REUSOCK_PORT];
} __attribute__((packed));

#endif
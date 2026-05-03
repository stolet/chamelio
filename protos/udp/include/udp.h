#ifndef UDP_H_
#define UDP_H_

#include <linux/types.h>

#include "cham_fast.h"
#include "utils.h"

#define UDP_APP_SOCKET_PATH "/run/chamelio/udp_app_socket"

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
/* Max number of ports to probe for an ephemeral bind in fast-path */
#define UDP_PORT_SCAN_MAX 8
/* Max number of scheduler entries */
#define MAX_SCHED MAX_SOCKETS
/* Used to signal that a socket entry is invalid/empty */
#define ID_INVALID (-1U)
/* Location where ebpf bytecode is located */
#define UDP_EBPF_BYTECODE "protos/udp/fast/udp_fast.bpf.o"
#define UDP_EBPF_BYTECODE_64 "protos/udp/fast/udp_fast_64.bpf.o"
#define UDP_EBPF_BYTECODE_128 "protos/udp/fast/udp_fast_128.bpf.o"
#define UDP_EBPF_BYTECODE_256 "protos/udp/fast/udp_fast_256.bpf.o"
#define UDP_EBPF_BYTECODE_512 "protos/udp/fast/udp_fast_512.bpf.o"
#define UDP_EBPF_BYTECODE_1024 "protos/udp/fast/udp_fast_1024.bpf.o"

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
  /* Queue IDs to bump app, indexed by fast-path core */
  __u16 app_bump_qids[MAX_FP_CORES];
  /* Core was learned from first received packet */
  __u8 core_learned;
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
} __attribute__((packed, aligned(64)));

/* Maps a port to a socket id */
struct udp_port {
  /* Number of sockets bounded to this port */
  __u32 nsocks;
  /* Next socket to load balance to if a message is received */
  __u32 next_sock;
  /* Socket IDs */
  __u32 sids[MAX_REUSOCK_PORT];
} __attribute__((packed));

struct udp_cfg {
  /* Next ephemeral port candidate for fast-path allocation */
  __u16 next_port;
  __u16 __pad;
} __attribute__((packed));

#endif

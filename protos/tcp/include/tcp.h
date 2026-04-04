#ifndef TCP_H_
#define TCP_H_

#include <linux/types.h>

#include "utils.h"

/* Maximum segment size for TCP */
#define TCP_MSS 1400
/* TODO: Pass MAX_APPS and MAX_CTXS as a parameter to TCP slow-path */
/* Max number of applications that can register with slow-path */
#define MAX_APPS 8
/* Max number of contexts per application */
#define MAX_CTXS 8
/* Minumum port number */
#define MIN_PORT 1002
/* Maximum port number */
#define MAX_PORT 65534
/* Maximum number of TCP sockets */
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
/* Number of hash buckets for TCP flow lookup in the fast-path */
#define TCP_FLOW_BUCKETS 4096
/* Number of entries per flow lookup bucket */
#define TCP_FLOW_BUCKET_SLOTS 4
/* Location where ebpf bytecode is located */
#define TCP_EBPF_BYTECODE "protos/tcp/fast/tcp_fast.bpf.o"

enum tcp_sock_state {
  TCP_SOCK_STATE_CLOSED = 0,
  TCP_SOCK_STATE_INIT,
  TCP_SOCK_STATE_LISTEN,
  TCP_SOCK_STATE_SYN_SENT,
  TCP_SOCK_STATE_SYN_RECV,
  TCP_SOCK_STATE_ACCEPT_PENDING,
  TCP_SOCK_STATE_ESTABLISHED,
  TCP_SOCK_STATE_FIN_WAIT1,
};

#define TCP_SOCK_FLAG_SHUT_RD 0x1
#define TCP_SOCK_FLAG_SHUT_WR 0x2

/* Entry for the socket map */
struct tcp_sock {
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
  /* Remote IP */
  __u32 remote_ip;

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
  
  /* Next sequence number to transmit */
  __u32 tx_seq;
  /* Sent unacked bytes */
  __u32 tx_pending;
  /* Next sequence number expected from peer */
  __u32 rx_seq;

  /* Local port */
  __u16 local_port;
  /* Remote port */
  __u16 remote_port;
  /* Socket state */
  __u8 state;
  /* 1 if this socket reuses a port 0 otherwise */
  __u8 reuport;
  /* Application ID that owns this socket */
  __u8 app_id;
  /* Application context ID that owns this socket */
  __u8 ctx_id;
  /* Socket flags */
  __u8 flags;
  
} __attribute__((packed));

/* Maps a listener port to one or more listening sockets */
struct tcp_port {
  /* Number of listening sockets on this port */
  __u32 nsocks;
  /* Next socket to load balance to if a message is received */
  __u32 next_sock;
  /* Socket IDs */
  __u32 sids[MAX_REUSOCK_PORT];
} __attribute__((packed));

/* Small fixed-size flow hash bucket used by fast-path lookups */
struct tcp_flow_bucket {
  /* Socket IDs for the flows in this bucket */
  __u32 sids[TCP_FLOW_BUCKET_SLOTS];
} __attribute__((packed));

/* Shared protocol configuration visible to the fast-path */
struct tcp_ctrl_cfg {
  /* Signal queue used by fast-path to punt control packets to slow-path */
  __u16 fast_slow_sig_qid;
  /* Packet queue used by fast-path to punt control packets to slow-path */
  __u16 fast_slow_pkt_qid;
  /* Signal queue used by slow-path to send control packets to fast-path */
  __u16 slow_fast_sig_qid;
  /* Packet queue used by slow-path to send control packets to fast-path */
  __u16 slow_fast_pkt_qid;
} __attribute__((packed));

#endif

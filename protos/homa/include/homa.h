#ifndef CHAM_HOMA_H
#define CHAM_HOMA_H

#include <linux/types.h>

/* Application registration socket. */
#define HOMA_APP_SOCKET_PATH "/run/chamelio/homa_app_socket"
/* Fast-path object relative to the build directory. */
#define HOMA_EBPF_PATH "protos/homa/fast/homa_fast.bpf.o"
/* Maximum endpoints in the initial single application context. */
#define HOMA_MAX_SOCKETS 64
/* Number of reserved RPC slots. */
#define HOMA_MAX_RPCS 256
/* Client slots; remaining slots are allocated by the fast path. */
#define HOMA_CLIENT_RPCS (HOMA_MAX_RPCS / 2)
/* Bytes reserved per message direction for single-packet bring-up. */
#define HOMA_MESSAGE_BYTES 2048
/* Entries in each application/control or application/fast-path queue. */
#define HOMA_QUEUE_LENGTH 128
/* Low wire-ID bit identifies the server side of an RPC. */
#define HOMA_RPC_SERVER_BIT 1ULL
/* Client wire IDs advance by two, reserving the low bit for the role. */
#define HOMA_RPC_ID_STRIDE 2ULL
/* Interval between requests for a missing completion ACK. */
#define HOMA_NEED_ACK_US 1000
/* Maximum RPC metadata entries examined per slow-path cleanup poll. */
#define HOMA_REAP_BATCH 8

/* Shared maps in creation order. */
enum homa_map_id {
  HOMA_PORT_MAP,
  HOMA_SOCKET_MAP,
  HOMA_RPC_MAP,
  HOMA_STATE_MAP,
};

/* Shared queues in creation order. */
enum homa_queue_id {
  HOMA_APP_SLOW_Q,
  HOMA_SLOW_APP_Q,
  HOMA_APP_FAST_Q,
  HOMA_FAST_APP_Q,
};

/* Endpoint lifecycle, owned by the slow path. */
enum homa_socket_state {
  HOMA_SOCKET_FREE,
  HOMA_SOCKET_OPEN,
  HOMA_SOCKET_BOUND,
  HOMA_SOCKET_CLOSING,
  HOMA_SOCKET_CLOSED,
};

/* Single-core server RPC allocation and lookup. */
struct homa_state {
  /* Number of available server slots. */
  __u32 server_free_count;
  /* Stack of available server slot indices. */
  __u32 server_free[HOMA_CLIENT_RPCS];
  /* Server RPC hash bucket heads, stored as slot index plus one. */
  __u32 server_buckets[HOMA_CLIENT_RPCS];
};

/* Endpoint state shared with the fast path. */
struct homa_socket {
  /* Host-order local IPv4 address. */
  __u32 local_ip;
  /* Host-order local port; zero until bound. */
  __u16 local_port;
  /* Endpoint lifecycle state. */
  __u16 state;
  /* Notification queue belonging to the endpoint's application context. */
  __u32 app_qid;
};

/* RPC transport lifecycle; application ownership is tracked separately. */
enum homa_rpc_state {
  HOMA_RPC_FREE,
  HOMA_RPC_WAIT_RESPONSE,
  HOMA_RPC_REQUEST_READY,
  HOMA_RPC_RESPONSE_SENT,
  HOMA_RPC_RESPONSE_READY,
  HOMA_RPC_RESPONSE_ACKED,
};

/* Application has released the received message. */
#define HOMA_RPC_RX_RELEASED 0x1

/* Shared RPC state and retained message storage. */
struct homa_rpc {
  /* Local handle; advances by HOMA_MAX_RPCS each time this slot is reused. */
  __u64 handle;
  /* Local wire ID, with the server role encoded in bit zero. */
  __u64 id;
  /* Owning endpoint index. */
  __u32 sock_id;
  /* Remote IPv4 address in host byte order. */
  __u32 remote_ip;
  /* Remote port in host byte order. */
  __u16 remote_port;
  /* RPC lifecycle. */
  __u16 state;
  /* Receive ownership flags. */
  __u16 flags;
  /* Original application's notification queue, preserved on reconnect. */
  __u32 app_qid;
  /* Bytes retained for transmission. */
  __u32 tx_len;
  /* Complete received message bytes. */
  __u32 rx_len;
  /* Previous server hash entry plus one; zero means none. */
  __u32 hash_prev;
  /* Next server hash entry plus one; zero means none. */
  __u32 hash_next;
  /* Transmit message storage owned by the transport. */
  __u8 tx_data[HOMA_MESSAGE_BYTES];
  /* Receive message storage held until application release. */
  __u8 rx_data[HOMA_MESSAGE_BYTES];
};

#endif

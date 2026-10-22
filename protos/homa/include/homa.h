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
/* Bytes reserved per message direction for single-packet bring-up. */
#define HOMA_MESSAGE_BYTES 2048
/* Entries in each application/control or application/fast-path queue. */
#define HOMA_QUEUE_LENGTH 128
/* Low wire-ID bit identifies the server side of an RPC. */
#define HOMA_RPC_SERVER_BIT 1ULL
/* Client wire IDs advance by two, reserving the low bit for the role. */
#define HOMA_RPC_ID_STRIDE 2ULL

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
};

/* Single-core allocation state for the bounded step 2 run. */
struct homa_state {
  /* Number of incoming RPC slots consumed by the fast path. */
  __u32 incoming_rpcs;
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

/* RPC lifecycle; completed slots remain retained until step 3. */
enum homa_rpc_state {
  HOMA_RPC_FREE,
  HOMA_RPC_WAIT_RESPONSE,
  HOMA_RPC_REQUEST_READY,
  HOMA_RPC_RESPONSE_SENT,
  HOMA_RPC_RESPONSE_READY,
};

/* Half the slots are application allocated, half fast-path allocated. */
#define HOMA_CLIENT_RPCS (HOMA_MAX_RPCS / 2)

/* Shared RPC state and retained message storage. */
struct homa_rpc {
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
  /* Original application's notification queue, preserved on reconnect. */
  __u32 app_qid;
  /* Bytes retained for transmission. */
  __u32 tx_len;
  /* Complete received message bytes. */
  __u32 rx_len;
  /* Transmit message storage owned by the transport. */
  __u8 tx_data[HOMA_MESSAGE_BYTES];
  /* Receive message storage held until application release. */
  __u8 rx_data[HOMA_MESSAGE_BYTES];
};

#endif

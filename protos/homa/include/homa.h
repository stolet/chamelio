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

/* Shared maps in creation order. */
enum homa_map_id {
  HOMA_PORT_MAP,
  HOMA_SOCKET_MAP,
  HOMA_RPC_MAP,
};

/* Endpoint lifecycle, owned by the slow path. */
enum homa_socket_state {
  HOMA_SOCKET_FREE,
  HOMA_SOCKET_OPEN,
  HOMA_SOCKET_BOUND,
};

/* Endpoint state shared with the fast path. */
struct homa_socket {
  /* Host-order local IPv4 address. */
  __u32 local_ip;
  /* Host-order local port; zero until bound. */
  __u16 local_port;
  /* Endpoint lifecycle state. */
  __u16 state;
};

/* Reserved RPC slot; packet processing will activate these in step 2. */
struct homa_rpc {
  /* RPC identifier; zero-initialized until the slot is used. */
  __u64 id;
  /* Transmit message storage owned by the transport. */
  __u8 tx_data[HOMA_MESSAGE_BYTES];
  /* Receive message storage held until application release. */
  __u8 rx_data[HOMA_MESSAGE_BYTES];
};

#endif

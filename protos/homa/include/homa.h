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
/* Maximum message bytes and reserved storage per direction. */
#define HOMA_MESSAGE_BYTES 1000000
/* DATA payload at the physical MTU of 1500 bytes. */
#define HOMA_MAX_PAYLOAD 1420
/* Words needed to track all fixed-size message segments. */
#define HOMA_BITMAP_WORDS \
  ((HOMA_MESSAGE_BYTES + HOMA_MAX_PAYLOAD * 32 - 1) / \
      (HOMA_MAX_PAYLOAD * 32))
/* Default initial bytes sent without a grant. */
#define HOMA_UNSCHEDULED_BYTES 16384
/* Default receive window extended by grants. */
#define HOMA_GRANT_BYTES 16384
/* Default cap on known incoming bytes when issuing additional grants. */
#define HOMA_INCOMING_BYTES 65536
/* Scheduler ID reserved for receiver grant work. */
#define HOMA_GRANT_SCHED_ID HOMA_MAX_RPCS
/* Effective initial network priority in the eTRAN artifact. */
#define HOMA_GRANT_PRIORITY 7
/* Highest scheduled priority; unscheduled DATA uses priority seven. */
#define HOMA_MAX_SCHED_PRIORITY 3
/* Peers considered in one grant round. */
#define HOMA_OVERCOMMITMENT 4
/* Percentage of byte service reserved for the oldest eligible RPC. */
#define HOMA_FIFO_PERCENT 1
/* Default aggregate physical link pacing rate in kilobits per second. */
#define HOMA_LINK_KBPS 100000000
/* Ethernet header, FCS, preamble and interpacket gap on the physical wire. */
#define HOMA_WIRE_OVERHEAD 38
/* IPv4 precedence occupies the top three TOS bits. */
#define HOMA_PRIORITY_SHIFT 5
/* Words in the receiver's grant eligibility bitmap. */
#define HOMA_CANDIDATE_WORDS (HOMA_MAX_RPCS / 32)
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

/* Compact receiver selection metadata, kept separate from message storage. */
struct homa_grant_candidate {
  /* Unique bytes left to receive. */
  __u32 remaining;
  /* Additional bytes the receive window currently permits. */
  __u32 need;
  /* Remote IPv4 address identifying the peer. */
  __u32 peer;
};

/* Single-core scheduling, server RPC allocation and lookup. */
struct homa_state {
  /* Aggregate DATA pacing rate. */
  __u32 link_kbps;
  /* Next DATA eligibility time in TSC cycles. */
  __u64 tx_ready_tsc;
  /* First pending transmit in admission order, including credit waits. */
  __u32 tx_fifo_head;
  /* Last pending transmit in admission order. */
  __u32 tx_fifo_tail;
  /* Sender byte credits toward oldest-message service. */
  __u64 tx_fifo_credit;
  /* Receiver byte credits toward oldest-message service. */
  __u64 grant_fifo_credit;
  /* Peers already granted in this round. */
  __u32 grant_peers[HOMA_OVERCOMMITMENT];
  /* Number of peers already granted in this round. */
  __u32 grant_count;
  /* Eligible grant candidates, one bit per RPC slot. */
  __u32 grant_bits[HOMA_CANDIDATE_WORDS];
  /* Hot selection metadata for pending receives. */
  struct homa_grant_candidate candidates[HOMA_MAX_RPCS];
  /* Configured initial transmit allowance. */
  __u32 unscheduled_bytes;
  /* Configured receive grant window. */
  __u32 grant_bytes;
  /* Configured receiver incoming-byte budget. */
  __u32 incoming_limit;
  /* Known bytes authorized but not yet received, including unscheduled DATA. */
  __u32 incoming;
  /* First pending receive, slot plus one. */
  __u32 grant_head;
  /* Last grant candidate, slot plus one. */
  __u32 grant_tail;
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
  HOMA_RPC_REQUEST_RECEIVING,
  HOMA_RPC_REQUEST_READY,
  HOMA_RPC_RESPONSE_SENT,
  HOMA_RPC_RESPONSE_READY,
  HOMA_RPC_RESPONSE_ACKED,
};

/* Application has released the received message. */
#define HOMA_RPC_RX_RELEASED 0x1
/* More DATA segments remain eligible for transmission. */
#define HOMA_RPC_TX_PENDING 0x2
/* Receive accounting has been initialized for this message. */
#define HOMA_RPC_RX_ACTIVE 0x4
/* RPC belongs to the receiver's pending grant queue. */
#define HOMA_RPC_GRANT_QUEUED 0x8
/* RPC belongs to the eligible DATA queue. */
#define HOMA_RPC_TX_QUEUED 0x10
/* RPC belongs to the transmit admission-order queue. */
#define HOMA_RPC_TX_FIFO_QUEUED 0x20

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
  /* Transport progress and receive ownership flags. */
  __u16 flags;
  /* Original application's notification queue, preserved on reconnect. */
  __u32 app_qid;
  /* Bytes retained for transmission. */
  __u32 tx_len;
  /* Next message byte to transmit. */
  __u32 tx_offset;
  /* Cumulative byte offset authorized for transmission. */
  __u32 tx_credit;
  /* Initial unscheduled boundary, preserved as grants increase tx_credit. */
  __u32 tx_unscheduled;
  /* Receiver-selected scheduled DATA priority. */
  __u8 tx_priority;
  /* Previous pending transmit in admission order, slot plus one. */
  __u32 tx_fifo_prev;
  /* Next pending transmit in admission order, slot plus one. */
  __u32 tx_fifo_next;
  /* Total message bytes, fixed by the first received segment. */
  __u32 rx_len;
  /* Received fixed-size segment indices; the final segment may be shorter. */
  __u32 rx_bitmap[HOMA_BITMAP_WORDS];
  /* Unique received payload bytes. */
  __u32 rx_received;
  /* Cumulative receive offset authorized by unscheduled DATA or grants. */
  __u32 rx_credit;
  /* Previous grant candidate slot plus one. */
  __u32 grant_prev;
  /* Next grant candidate slot plus one. */
  __u32 grant_next;
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

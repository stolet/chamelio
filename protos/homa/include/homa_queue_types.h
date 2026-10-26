#ifndef CHAM_HOMA_QUEUE_TYPES_H
#define CHAM_HOMA_QUEUE_TYPES_H

#include <linux/types.h>

#include "utils.h"

/* Application control and data-path messages. */
enum homa_queue_type {
  /* Signals that the queue is empty. */
  HOMA_QUEUE_EMPTY = 0,
  /* Request to create an endpoint. */
  HOMA_QUEUE_SOCKET,
  /* Request to bind an endpoint. */
  HOMA_QUEUE_BIND,
  /* Request to close an endpoint. */
  HOMA_QUEUE_CLOSE,
  /* Result of an endpoint control request. */
  HOMA_QUEUE_RESULT,
  /* Transmit the retained request or response in an RPC slot. */
  HOMA_QUEUE_TX,
  /* Deliver a complete received message from an RPC slot. */
  HOMA_QUEUE_RX,
  /* Release an application receive buffer. */
  HOMA_QUEUE_RELEASE,
  /* Abort and reclaim an RPC when its endpoint closes. */
  HOMA_QUEUE_REAP,
  /* Request completion confirmation for an unacknowledged response. */
  HOMA_QUEUE_NEED_ACK,
  /* Request a missing DATA range or probe peer state. */
  HOMA_QUEUE_RESEND,
  /* Execute a retained DATA retransmission chosen by the slow path. */
  HOMA_QUEUE_RETRANSMIT,
  /* Restore lost transmit credit and respond BUSY. */
  HOMA_QUEUE_CREDIT,
  /* Fail an RPC that its peer no longer recognizes. */
  HOMA_QUEUE_ABORT,
  /* Send a BUSY or UNKNOWN response chosen by the slow path. */
  HOMA_QUEUE_CONTROL,
  /* Received RESEND packet metadata for the slow path. */
  HOMA_QUEUE_RX_RESEND,
  /* Received BUSY packet metadata for the slow path. */
  HOMA_QUEUE_RX_BUSY,
  /* Received UNKNOWN packet metadata for the slow path. */
  HOMA_QUEUE_RX_UNKNOWN,
  /* Fail an RPC after unanswered recovery probes. */
  HOMA_QUEUE_FAIL,
  /* Deliver an RPC failure to the application. */
  HOMA_QUEUE_ERROR,
  /* Confirm that the application consumed an RPC failure. */
  HOMA_QUEUE_ERROR_REPORTED,
  /* Stop new RPC admission before slow-path cleanup examines the table. */
  HOMA_QUEUE_CLOSE_START,
  /* Mark an endpoint closed after its preceding cleanup commands. */
  HOMA_QUEUE_CLOSE_DONE,
};

/* Endpoint control request. */
struct homa_queue_request {
  /* Endpoint index returned by socket creation. */
  __u32 sock_id;
  /* Host-order local IPv4 address for bind. */
  __u32 local_ip;
  /* Host-order local port; zero requests an ephemeral port. */
  __u16 local_port;
} __attribute__((packed));

/* Result of an endpoint control request. */
struct homa_queue_result {
  /* Endpoint index, including newly created endpoints. */
  __u32 sock_id;
  /* Zero on success, otherwise a positive errno. */
  __s32 status;
} __attribute__((packed));

/* Slow-path recovery decision, validated against current fast-path progress. */
struct homa_queue_recovery {
  /* Generation-aware local RPC handle. */
  __u64 rpc_handle;
  /* Progress counter observed when the command was prepared. */
  __u64 progress;
  /* First missing byte; zero for probes. */
  __u32 offset;
  /* Missing byte count; zero for liveness-only probes. */
  __u32 length;
  /* Cumulative transmit credit authorized by the receiver. */
  __u32 credit;
  /* Priority for retransmitted DATA. */
  __u8 priority;
} __attribute__((packed));

/* Recovery packet metadata; commands also identify the current endpoint. */
struct homa_queue_packet {
  /* Local wire RPC ID, with the receiving endpoint's role bit. */
  __u64 id;
  /* Local IPv4 address in host byte order. */
  __u32 local_ip;
  /* Remote IPv4 address in host byte order. */
  __u32 remote_ip;
  /* Requested first byte for RESEND. */
  __u32 offset;
  /* Requested byte count for RESEND. */
  __u32 length;
  /* Endpoint index, filled by the slow path for control replies. */
  __u32 sock_id;
  /* Local port in host byte order. */
  __u16 local_port;
  /* Remote port in host byte order. */
  __u16 remote_port;
  /* Requested retransmission priority. */
  __u8 priority;
  /* Wire packet type for a control reply. */
  __u8 type;
} __attribute__((packed));

/* Cache-line queue entry for endpoint control or RPC notification. */
struct homa_queue_entry {
  /* Type of queue entry. Don't update outside of enqueue or dequeue */
  volatile __u8 type;
  /* Request arguments or response fields. */
  union {
    /* Endpoint control request. */
    struct homa_queue_request request;
    /* Result of the preceding control request. */
    struct homa_queue_result result;
    /* Generation-aware RPC handle for commands and notifications. */
    __u64 rpc_handle;
    /* Slow-path timeout or retransmission decision. */
    struct homa_queue_recovery recovery;
    /* Received recovery packet or stateless control response. */
    struct homa_queue_packet packet;
    /* Keep entries one cache line wide. */
    __u8 raw[63];
  } __attribute__((packed)) data;
} __attribute__((packed, aligned(64)));

/* Shared resource offsets sent when the application context attaches. */
struct homa_context_info {
  /* Mapped length of the protocol's shared memory. */
  __u64 shm_len;
  /* File offset of the protocol's shared memory. */
  __u64 shm_off;
  /* Shared endpoint table offset. */
  __u64 sockets_off;
  /* Shared RPC table offset. */
  __u64 rpcs_off;
  /* Application-to-slow-path queue offset. */
  __u64 app_slow_off;
  /* Slow-path-to-application queue offset. */
  __u64 slow_app_off;
  /* Application-to-fast-path queue offset. */
  __u64 app_fast_off;
  /* Fast-path-to-application queue offset. */
  __u64 fast_app_off;
};

STATIC_ASSERT(sizeof(struct homa_queue_entry) == 64, homa_queue_entry_size);

#endif

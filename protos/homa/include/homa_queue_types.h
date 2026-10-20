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
    /* RPC slot index for data-path commands and notifications. */
    __u32 rpc_slot;
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

#ifndef CHAM_HOMA_SLOW_H
#define CHAM_HOMA_SLOW_H

#include "cham_lib.h"
#include "homa.h"
#include "homa_config.h"
#include "homa_queue_types.h"

/* Slow-path observation and retry state for one RPC generation. */
struct homa_rpc_timer {
  /* Local RPC generation being observed. */
  __u64 handle;
  /* Monotonic nanosecond deadline for the next probe. */
  __u64 deadline;
  /* Last observed fast-path progress counter. */
  __u64 progress;
  /* Probes sent without intervening progress. */
  __u32 attempts;
};

/* Application attachment and endpoint cleanup state. */
struct homa_app {
  /* Application attachment response with shared queue locations. */
  struct homa_context_info info;
  /* Application-to-slow-path queue view. */
  struct dqueue app_slow;
  /* Slow-path-to-application queue view. */
  struct equeue slow_app;
  /* Endpoint currently being cleaned, or -1. */
  int closing_sock;
  /* Next RPC slot to inspect for the current close. */
  __u32 close_cursor;
  /* Cleanup barrier has been submitted to the fast path. */
  int close_barrier;
  /* Application disconnected; finish cleanup before reusing this slot. */
  int disconnecting;
  /* Active application-to-fast-path queue ID. */
  __u16 app_fast_qid;
  /* Active fast-path-to-application queue ID. */
  __u16 fast_app_qid;
  /* Context-reset command has been submitted. */
  int reset_pending;
  /* Active application connection, or -1. */
  int app_fd;
};

/* Protocol registration and application control state. */
struct homa_slow {
  /* Slow-path configuration. */
  struct homa_configuration config;
  /* Registered protocol and shared-memory mapping. */
  struct proto_lib *proto;
  /* Port to endpoint index plus one; zero means unbound. */
  __u32 *ports;
  /* Shared endpoint table. */
  struct homa_socket *sockets;
  /* Shared RPC table. */
  struct homa_rpc *rpcs;
  /* Shared attachment ownership and reset barriers. */
  struct homa_context *contexts;
  /* Cleanup and recovery commands for core zero. */
  struct equeue slow_fast;
  /* Received recovery metadata from core zero. */
  struct dqueue fast_slow;
  /* Application attachments with reserved RPC slot ranges. */
  struct homa_app apps[HOMA_MAX_CONTEXTS];
  /* Slow-path observations for each RPC generation. */
  struct homa_rpc_timer timers[HOMA_MAX_RPCS];
  /* Listening application socket. */
  int listen_fd;
};

/* Register Homa, allocate shared resources, and upload the fast path. */
int homa_init_context(struct homa_slow *ctx);
/* Open the local application registration socket. */
int homa_app_init(struct homa_slow *ctx);
/* Process recovery packets and submit decisions to the fast path. */
void homa_recovery_poll(struct homa_slow *ctx);
/* Detect stalled RPCs and submit recovery, completion, or failure commands. */
void homa_timer_poll(struct homa_slow *ctx);
/* Accept applications, detect disconnects, and process endpoint requests. */
int homa_app_poll(struct homa_slow *ctx);

#endif

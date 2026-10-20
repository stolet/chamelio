#ifndef CHAM_HOMA_SLOW_H
#define CHAM_HOMA_SLOW_H

#include "cham_lib.h"
#include "homa.h"
#include "homa_config.h"
#include "homa_queue_types.h"

/* Single-core protocol registration and application control state. */
struct homa_slow {
  /* Slow-path configuration */
  struct homa_configuration config;

  /* Registered Chamelio protocol and its shared-memory mapping. */
  struct proto_lib *proto;
  /* Port map: zero means free, otherwise endpoint index plus one. */
  __u32 *ports;
  /* Shared endpoint table. */
  struct homa_socket *sockets;
  /* Application attachment response with shared queue locations. */
  struct homa_context_info info;
  /* Application-to-slow-path queue view. */
  struct dqueue app_slow;
  /* Slow-path-to-application queue view. */
  struct equeue slow_app;
  /* Listening application socket. */
  int listen_fd;
  /* Active application connection, or -1. */
  int app_fd;
};

/* Register Homa, allocate shared resources, and upload the fast path. */
int homa_init_context(struct homa_slow *ctx);
/* Open the local application registration socket. */
int homa_app_init(struct homa_slow *ctx);
/* Accept applications, detect disconnects, and process endpoint requests. */
int homa_app_poll(struct homa_slow *ctx);

#endif

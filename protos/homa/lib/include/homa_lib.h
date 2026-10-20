#ifndef CHAM_HOMA_LIB_H
#define CHAM_HOMA_LIB_H

#include <linux/types.h>
#include <netinet/in.h>
#include <stddef.h>
#include <sys/socket.h>

/* Single-threaded application context; private to the library. */
struct homa_context_lib;

/* Completion delivered by homa_lib_poll once packet support is implemented. */
enum homa_event_type {
  HOMA_EVENT_REQUEST,
  HOMA_EVENT_RESPONSE,
  HOMA_EVENT_ERROR,
};

/* Complete RPC message or failure delivered to the application. */
struct homa_event {
  /* Request, response, or failure. */
  enum homa_event_type type;
  /* Local endpoint that owns this event. */
  int sockfd;
  /* RPC handle used for reply and message release. */
  __u64 rpc_id;
  /* Remote endpoint in network byte order. */
  struct sockaddr_in peer;
  /* Message bytes, valid until homa_lib_release. */
  const void *data;
  /* Number of message bytes. */
  size_t len;
  /* Zero for messages, positive errno for failures. */
  int status;
};

/* Connect to the Homa slow path; one connection per process in step 1. */
int homa_lib_connect_slow(void);
/* Attach the single application context and map its queues. */
struct homa_context_lib *homa_lib_ctx_new(void);
/* Release the context and its endpoints, then disconnect from the slow path. */
void homa_lib_ctx_free(struct homa_context_lib *ctx);
/* Close an attachment, including one whose context creation failed. */
void homa_lib_disconnect_slow(void);
/* Create an endpoint; return its nonnegative descriptor or -1 with errno. */
int homa_lib_socket(struct homa_context_lib *ctx);
/* Bind an IPv4 endpoint; port zero selects an ephemeral port. */
int homa_lib_bind(struct homa_context_lib *ctx, int sockfd,
    const struct sockaddr *addr, socklen_t addrlen);
/* Close an endpoint and release its port. */
int homa_lib_close(struct homa_context_lib *ctx, int sockfd);
/* Submit a request; step 1 returns -1 with EOPNOTSUPP. */
int homa_lib_request(struct homa_context_lib *ctx, int sockfd, const void *data,
    size_t len, const struct sockaddr *peer, socklen_t peerlen, __u64 *rpc_id);
/* Submit a reply to a received RPC; step 1 returns -1 with EOPNOTSUPP. */
int homa_lib_reply(struct homa_context_lib *ctx, int sockfd, __u64 rpc_id,
    const void *data, size_t len);
/* Poll without blocking; step 1 has no network completions and returns zero. */
int homa_lib_poll(struct homa_context_lib *ctx, struct homa_event *events,
    int maxevents);
/* Release a received message; step 1 returns -1 with EOPNOTSUPP. */
int homa_lib_release(struct homa_context_lib *ctx, int sockfd, __u64 rpc_id);

#endif

#ifndef TCP_SLOW_INTERNAL_H_
#define TCP_SLOW_INTERNAL_H_

#include "tcp_slow.h"
#include "tcp_queue_types.h"

enum tcp_slow_timeout_type {
  TCP_TO_RETX = 1,
};

enum tcp_slow_retx_kind {
  TCP_SLOW_RETX_NONE = 0,
  TCP_SLOW_RETX_SYN,
  TCP_SLOW_RETX_SYNACK,
  TCP_SLOW_RETX_FIN,
};

#define TCP_SLOW_RETX_US 1000
#define TCP_SLOW_RETX_RETRIES 3

static inline struct tcp_sock *tcp_slow_get_sock_map(
    struct tcp_slow_context *ctx)
{
  return ctx->proto->shm_base + ctx->socks_map->off;
}

static inline struct tcp_port *tcp_slow_get_listener_map(
    struct tcp_slow_context *ctx)
{
  return ctx->proto->shm_base + ctx->port_map->off;
}

static inline struct tcp_flow_bucket *tcp_slow_get_flow_map(
    struct tcp_slow_context *ctx)
{
  return ctx->proto->shm_base + ctx->flow_map->off;
}

static inline struct tcp_ctrl_cfg *tcp_slow_get_ctrl_cfg(
    struct tcp_slow_context *ctx)
{
  return ctx->proto->shm_base + ctx->ctrl_map->off;
}

static inline struct tcp_app_context_slow *tcp_slow_sock_actx(
    struct tcp_slow_context *ctx, const struct tcp_sock *sock)
{
  return &ctx->apps[sock->app_id].ctxs[sock->ctx_id];
}

static inline const char *tcp_slow_state_name(__u8 state)
{
  switch (state)
  {
    case TCP_SOCK_STATE_CLOSED:
      return "CLOSED";
    case TCP_SOCK_STATE_INIT:
      return "INIT";
    case TCP_SOCK_STATE_LISTEN:
      return "LISTEN";
    case TCP_SOCK_STATE_SYN_SENT:
      return "SYN_SENT";
    case TCP_SOCK_STATE_SYN_RECV:
      return "SYN_RECV";
    case TCP_SOCK_STATE_ACCEPT_PENDING:
      return "ACCEPT_PENDING";
    case TCP_SOCK_STATE_ESTABLISHED:
      return "ESTABLISHED";
    case TCP_SOCK_STATE_FIN_WAIT1:
      return "FIN_WAIT1";
    default:
      return "UNKNOWN";
  }
}

int tcp_slow_init(struct tcp_slow_context *ctx);
int tcp_slow_app_poll(struct tcp_slow_context *ctx);
int tcp_slow_fast_poll(struct tcp_slow_context *ctx);
int tcp_slow_cc_poll(struct tcp_slow_context *ctx);
int tcp_slow_timeout_poll(struct tcp_slow_context *ctx);
int tcp_slow_timeout_arm(struct tcp_slow_context *ctx, struct tcp_sock *sock,
    __u8 kind);
void tcp_slow_timeout_cancel(struct tcp_slow_context *ctx,
    struct tcp_sock *sock);
int tcp_slow_cc_ecn_enabled(const struct tcp_slow_context *ctx);
void tcp_slow_cc_init_sock(struct tcp_slow_context *ctx, struct tcp_sock *sock);
void tcp_slow_cc_reset_sock(struct tcp_slow_context *ctx, struct tcp_sock *sock);

int tcp_slow_state_alloc_sock(struct tcp_slow_context *ctx, __u64 opaque,
    __u8 app_id, __u8 ctx_id, __u16 app_bump_qid, struct tcp_sock **sock_out);
int tcp_slow_state_fill_new_sock_res(struct tcp_sock *sock,
    struct tcp_queue_new_sock_res *res);
int tcp_slow_state_fill_accept_res(struct tcp_sock *listen_sock,
    struct tcp_sock *sock, struct tcp_queue_accept_res *res);
__u16 tcp_slow_state_rx_window(const struct tcp_sock *sock);
int tcp_slow_state_bound_insert(struct tcp_slow_context *ctx, __u16 port,
    __u32 sock_id, int reuseport);
void tcp_slow_state_bound_remove(struct tcp_slow_context *ctx, __u16 port,
    __u32 sock_id);
__u16 tcp_slow_state_bound_find_free_port(struct tcp_slow_context *ctx);
int tcp_slow_state_listener_insert(struct tcp_slow_context *ctx, __u16 port,
    __u32 sock_id, int reuseport);
void tcp_slow_state_listener_remove(struct tcp_slow_context *ctx, __u16 port,
    __u32 sock_id);
__u32 tcp_slow_state_listener_lookup(struct tcp_slow_context *ctx,
    __u32 local_ip, __u16 port);
void tcp_slow_state_listener_reset(struct tcp_slow_context *ctx,
    __u32 listener_id);
void tcp_slow_state_listener_detach_child(struct tcp_slow_context *ctx,
    struct tcp_sock *sock);
int tcp_slow_state_listener_ready_push(struct tcp_slow_context *ctx,
    __u32 listener_id, __u32 sock_id);
struct tcp_sock *tcp_slow_state_listener_ready_pop(
    struct tcp_slow_context *ctx, __u32 listener_id);
void tcp_slow_state_listener_abort_children(struct tcp_slow_context *ctx,
    struct tcp_sock *listen_sock);
int tcp_slow_state_flow_insert(struct tcp_slow_context *ctx,
    struct tcp_sock *sock);
void tcp_slow_state_flow_remove(struct tcp_slow_context *ctx,
    struct tcp_sock *sock);
struct tcp_sock *tcp_slow_state_flow_lookup(struct tcp_slow_context *ctx,
    __u32 local_ip, __u16 local_port, __u32 remote_ip, __u16 remote_port);
int tcp_slow_state_enqueue_ctrl_tx(struct tcp_slow_context *ctx,
    struct tcp_sock *sock, __u16 flags);
int tcp_slow_state_enqueue_ctrl_resend(struct tcp_slow_context *ctx,
    struct tcp_sock *sock, __u16 flags);
int tcp_slow_state_enqueue_tx_sched(struct tcp_slow_context *ctx,
    struct tcp_sock *sock);
int tcp_slow_state_enqueue_tx_retransmit(struct tcp_slow_context *ctx,
    struct tcp_sock *sock);
int tcp_slow_state_enqueue_ctrl_reply(struct tcp_slow_context *ctx,
    __u32 local_ip, __u16 local_port, __u32 remote_ip, __u16 remote_port,
    __u32 seq, __u32 ack, __u16 flags);
void tcp_slow_state_sock_close_final(struct tcp_slow_context *ctx,
    struct tcp_sock *sock);
void tcp_slow_state_sock_connect_failed(struct tcp_slow_context *ctx,
    struct tcp_sock *sock, int err);

int tcp_slow_app_enqueue_listen_res(struct tcp_app_context_slow *actx,
    __u64 opaque, int status);
int tcp_slow_app_enqueue_connect_res(struct tcp_app_context_slow *actx,
    __u64 opaque, int status, struct tcp_sock *sock);
int tcp_slow_app_enqueue_accept_res(struct tcp_app_context_slow *actx,
    struct tcp_sock *listen_sock, struct tcp_sock *sock, int status);
int tcp_slow_app_enqueue_listen_newconn(struct tcp_slow_context *ctx,
    struct tcp_sock *listen_sock, struct tcp_sock *sock);

#endif

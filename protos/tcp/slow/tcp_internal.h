#ifndef TCP_INTERNAL_H_
#define TCP_INTERNAL_H_

#include "tcp_slow.h"
#include "tcp_queue_types.h"

/*** Enums ********************************************************************/

enum tcp_timeout_type {
  TCP_TO_RETX = 1,
};

enum tcp_retx_kind {
  TCP_RETX_NONE = 0,
  TCP_RETX_SYN,
  TCP_RETX_SYNACK,
  TCP_RETX_FIN,
};

/*** Types ********************************************************************/

struct tcp_rx_ctl {
  __u32 local_ip;
  __u32 remote_ip;
  __u32 seq;
  __u32 ack;
  __u16 local_port;
  __u16 remote_port;
  __u16 wnd;
  __u16 flags;
};

/*** Inline Helpers ***********************************************************/

#define TCP_RETX_US 1000
#define TCP_RETX_RETRIES 3

static inline struct tcp_sock *tcp_sock_map(struct tcp_slow_context *ctx)
{
  return ctx->proto->shm_base + ctx->socks_map->off;
}

static inline struct tcp_port *tcp_listener_map(struct tcp_slow_context *ctx)
{
  return ctx->proto->shm_base + ctx->port_map->off;
}

static inline struct tcp_flow_bucket *tcp_flow_map(struct tcp_slow_context *ctx)
{
  return ctx->proto->shm_base + ctx->flow_map->off;
}

static inline struct tcp_ctl_cfg *tcp_ctl_cfg_map(struct tcp_slow_context *ctx)
{
  return ctx->proto->shm_base + ctx->ctl_map->off;
}

static inline struct tcp_sock_meta_slow *tcp_sock_meta(
    struct tcp_slow_context *ctx, const struct tcp_sock *sock)
{
  return &ctx->sock_meta[sock->id];
}

static inline struct tcp_app_context_slow *tcp_sock_actx(
    struct tcp_slow_context *ctx, const struct tcp_sock *sock)
{
  return &ctx->apps[sock->app_id].ctxs[sock->ctx_id];
}

static inline const char *tcp_sock_state_name(__u8 state)
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

/*** Public API ***************************************************************/

int tcp_init(struct tcp_slow_context *ctx);
int tcp_app_poll(struct tcp_slow_context *ctx);
int tcp_fast_poll(struct tcp_slow_context *ctx);
int tcp_cc_poll(struct tcp_slow_context *ctx);

/*** Timeout API **************************************************************/

int tcp_timeout_poll(struct tcp_slow_context *ctx);
int tcp_timeout_arm(struct tcp_slow_context *ctx, struct tcp_sock *sock,
    __u8 kind);
void tcp_timeout_cancel(struct tcp_slow_context *ctx, struct tcp_sock *sock);

/*** CC API *******************************************************************/

int tcp_cc_ecn_enabled(const struct tcp_slow_context *ctx);

/*** Socket API ***************************************************************/

void tcp_sock_cc_init(struct tcp_slow_context *ctx, struct tcp_sock *sock);
void tcp_sock_cc_reset(struct tcp_slow_context *ctx, struct tcp_sock *sock);
int tcp_sock_alloc(struct tcp_slow_context *ctx, __u64 opaque, __u8 app_id,
    __u8 ctx_id, __u16 app_bump_qid, struct tcp_sock **sock_out);
int tcp_sock_new_res_fill(struct tcp_sock *sock,
    struct tcp_queue_new_sock_res *res);
int tcp_sock_accept_res_fill(struct tcp_sock *listen_sock,
    struct tcp_sock *sock, struct tcp_queue_accept_res *res);
__u16 tcp_sock_rx_wnd(const struct tcp_sock *sock);
void tcp_sock_ack_progress(struct tcp_sock *sock);
void tcp_sock_close_final(struct tcp_slow_context *ctx, struct tcp_sock *sock);
void tcp_sock_connect_fail(struct tcp_slow_context *ctx, struct tcp_sock *sock,
    int err);

/*** Bound Ports **************************************************************/

int tcp_bound_insert(struct tcp_slow_context *ctx, __u16 port, __u32 sock_id,
    int reuseport);
void tcp_bound_remove(struct tcp_slow_context *ctx, __u16 port, __u32 sock_id);
__u16 tcp_bound_find_free(struct tcp_slow_context *ctx);

/*** Listener API *************************************************************/

int tcp_listener_insert(struct tcp_slow_context *ctx, __u16 port,
    __u32 sock_id, int reuseport);
void tcp_listener_remove(struct tcp_slow_context *ctx, __u16 port,
    __u32 sock_id);
__u32 tcp_listener_lookup(struct tcp_slow_context *ctx, __u32 local_ip,
    __u16 port);
void tcp_listener_reset(struct tcp_slow_context *ctx, __u32 listener_id);
void tcp_listener_detach_child(struct tcp_slow_context *ctx,
    struct tcp_sock *sock);
int tcp_listener_ready_push(struct tcp_slow_context *ctx, __u32 listener_id,
    __u32 sock_id);
struct tcp_sock *tcp_listener_ready_pop(struct tcp_slow_context *ctx,
    __u32 listener_id);
void tcp_listener_abort_children(struct tcp_slow_context *ctx,
    struct tcp_sock *listen_sock);

/*** Flow API *****************************************************************/

int tcp_flow_insert(struct tcp_slow_context *ctx, struct tcp_sock *sock);
void tcp_flow_remove(struct tcp_slow_context *ctx, struct tcp_sock *sock);
struct tcp_sock *tcp_flow_lookup(struct tcp_slow_context *ctx, __u32 local_ip,
    __u16 local_port, __u32 remote_ip, __u16 remote_port);

/*** Ctrl TX API **************************************************************/

int tcp_ctl_tx(struct tcp_slow_context *ctx, struct tcp_sock *sock,
    __u16 flags);
int tcp_ctl_tx_resend(struct tcp_slow_context *ctx, struct tcp_sock *sock,
    __u16 flags);
int tcp_ctl_tx_reply(struct tcp_slow_context *ctx, __u32 local_ip,
    __u16 local_port, __u32 remote_ip, __u16 remote_port, __u32 seq,
    __u32 ack, __u16 flags);
int tcp_tx_retransmit(struct tcp_slow_context *ctx, struct tcp_sock *sock);

/*** Response API *************************************************************/

int tcp_app_listen_res(struct tcp_app_context_slow *actx, __u64 opaque,
    int status);
int tcp_app_connect_res(struct tcp_app_context_slow *actx, __u64 opaque,
    int status, struct tcp_sock *sock);
int tcp_app_accept_res(struct tcp_app_context_slow *actx,
    struct tcp_sock *listen_sock, struct tcp_sock *sock, int status);
int tcp_app_listen_newconn(struct tcp_slow_context *ctx,
    struct tcp_sock *listen_sock, struct tcp_sock *sock);

/*** Open API *****************************************************************/

int tcp_rx_syn_sent(struct tcp_slow_context *ctx, struct tcp_sock *sock,
    const struct tcp_rx_ctl *rx);
int tcp_rx_syn_recv(struct tcp_slow_context *ctx, struct tcp_sock *sock,
    const struct tcp_rx_ctl *rx);
int tcp_rx_listen_syn(struct tcp_slow_context *ctx, struct tcp_sock *listen_sock,
    const struct tcp_rx_ctl *rx);

/*** Close API ****************************************************************/

int tcp_rx_established(struct tcp_slow_context *ctx, struct tcp_sock *sock,
    const struct tcp_rx_ctl *rx);
int tcp_rx_fin_wait1(struct tcp_slow_context *ctx, struct tcp_sock *sock,
    const struct tcp_rx_ctl *rx);

#endif

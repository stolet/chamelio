#include <stdlib.h>
#include <sys/socket.h>
#include <errno.h>

#include "tcp_internal.h"
#include "queue_fns.h"
#include "log.h"

/*** Request Handlers *********************************************************/

static int app_new_sock(struct tcp_slow_context *ctx,
    struct tcp_app_context_slow *actx, struct tcp_queue_entry *qe);
static int app_setopt(struct tcp_slow_context *ctx,
    struct tcp_app_context_slow *actx, struct tcp_queue_entry *qe);
static int app_bind(struct tcp_slow_context *ctx,
    struct tcp_app_context_slow *actx, struct tcp_queue_entry *qe);
static int app_connect(struct tcp_slow_context *ctx,
    struct tcp_app_context_slow *actx, struct tcp_queue_entry *qe);
static int app_listen(struct tcp_slow_context *ctx,
    struct tcp_app_context_slow *actx, struct tcp_queue_entry *qe);
static int app_accept(struct tcp_slow_context *ctx,
    struct tcp_app_context_slow *actx, struct tcp_queue_entry *qe);
static int app_shutdown(struct tcp_slow_context *ctx,
    struct tcp_app_context_slow *actx, struct tcp_queue_entry *qe);
static int app_close(struct tcp_slow_context *ctx,
    struct tcp_app_context_slow *actx, struct tcp_queue_entry *qe);
static int app_accept_err(struct tcp_app_context_slow *actx,
    struct tcp_sock *listen_sock, __u64 opaque, int err);

/*** Socket Helpers ***********************************************************/

static int sock_autobind_port(struct tcp_slow_context *ctx, struct tcp_sock *sock);
static int sock_autobind_ip(struct tcp_slow_context *ctx, struct tcp_sock *sock);
static int sock_connect_start(struct tcp_slow_context *ctx, struct tcp_sock *sock,
    __u32 remote_ip, __u16 remote_port);
static void sock_accept_own(struct tcp_slow_context *ctx, struct tcp_sock *sock,
    struct tcp_app_context_slow *actx, __u64 opaque);
static int sock_shutdown_start(struct tcp_slow_context *ctx,
    struct tcp_sock *sock);
static void sock_abort_open(struct tcp_slow_context *ctx, struct tcp_sock *sock);
static void sock_close_listen(struct tcp_slow_context *ctx,
    struct tcp_sock *sock);

/*** Request API **************************************************************/

int tcp_app_poll(struct tcp_slow_context *ctx)
{
  int msg_nr;
  int app_nr;
  int ctx_nr;
  __u8 type;
  struct dqueue *q;
  struct tcp_queue_entry *qe;
  struct tcp_app_slow *app;
  struct tcp_app_context_slow *actx;

  msg_nr = 0;
  app_nr = 0;
  ctx_nr = 0;
  while (msg_nr < SLOW_BATCH_SIZE && ctx->n_apps != 0)
  {
    app = &ctx->apps[ctx->next_app];
    if (ctx_nr >= app->n_ctxs)
    {
      app_nr++;
      ctx->next_app = (ctx->next_app + 1) % ctx->n_apps;
      app = &ctx->apps[ctx->next_app];
    }

    if (app_nr >= ctx->n_apps || app->n_ctxs == 0)
      break;

    actx = &app->ctxs[app->next_ctx];
    q = actx->app_slow_q;
    qe = queue_head(q);
    if (qe == NULL)
    {
      ctx_nr++;
      app->next_ctx = (app->next_ctx + 1) % app->n_ctxs;
      continue;
    }

    msg_nr++;
    type = qe->type;
    switch (type)
    {
      case TCP_QUEUE_EMPTY:
        break;
      case TCP_QUEUE_NEW_SOCK_REQ:
        app_new_sock(ctx, actx, qe);
        break;
      case TCP_QUEUE_SETOPT_REQ:
        app_setopt(ctx, actx, qe);
        break;
      case TCP_QUEUE_BIND_REQ:
        app_bind(ctx, actx, qe);
        break;
      case TCP_QUEUE_CONNECT_REQ:
        app_connect(ctx, actx, qe);
        break;
      case TCP_QUEUE_LISTEN_REQ:
        app_listen(ctx, actx, qe);
        break;
      case TCP_QUEUE_ACCEPT_REQ:
        app_accept(ctx, actx, qe);
        break;
      case TCP_QUEUE_SHUTDOWN_REQ:
        app_shutdown(ctx, actx, qe);
        break;
      case TCP_QUEUE_CLOSE_REQ:
        app_close(ctx, actx, qe);
        break;
      default:
        LOG_WARN("unknown queue entry type from app to tcp slow-path type=%d",
            type);
        break;
    }
    queue_dequeue(q);
  }

  return 0;
}

/*** Request Handlers *********************************************************/

static int app_new_sock(struct tcp_slow_context *ctx,
    struct tcp_app_context_slow *actx, struct tcp_queue_entry *qe)
{
  struct tcp_sock *sock;
  struct tcp_queue_entry *qe_res;
  struct tcp_queue_new_sock_req *req;

  req = &qe->data.new_sock_req;
  if (tcp_sock_alloc(ctx, req->opaque, actx->app->id, actx->id,
          actx->app_bump_qs[0]->id, &sock) != 0)
  {
    return -1;
  }

  qe_res = queue_tail(actx->slow_app_q);
  if (qe_res == NULL)
  {
    LOG_ERROR("failed to get tail of slow->app queue");
    return -1;
  }

  tcp_sock_new_res_fill(sock, &qe_res->data.new_sock_res);
  if (queue_enqueue(actx->slow_app_q, TCP_QUEUE_NEW_SOCK_RES) != 0)
  {
    LOG_ERROR("failed to enqueue new socket response");
    return -1;
  }

  return 0;
}

static int app_setopt(struct tcp_slow_context *ctx,
    struct tcp_app_context_slow *actx, struct tcp_queue_entry *qe)
{
  int success;
  struct tcp_sock *sock;
  struct tcp_queue_entry *qe_res;
  struct tcp_queue_setopt_req *req;
  struct tcp_queue_setopt_res *res;

  req = &qe->data.setopt_req;
  qe_res = queue_tail(actx->slow_app_q);
  if (qe_res == NULL)
  {
    LOG_ERROR("failed to get tail of slow->app queue");
    return -1;
  }

  res = &qe_res->data.setopt_res;
  res->opaque = req->opaque;
  success = 0;
  if (req->sock_id < ctx->n_socks)
  {
    sock = &tcp_sock_map(ctx)[req->sock_id];
    switch (req->opt)
    {
      case SO_REUSEPORT:
        sock->reuport = 1;
        success = 1;
        break;
      default:
        break;
    }
  }

  res->success = success;
  if (queue_enqueue(actx->slow_app_q, TCP_QUEUE_SETOPT_RES) != 0)
  {
    LOG_ERROR("failed to enqueue setopt response");
    return -1;
  }

  return success ? 0 : -1;
}

static int app_bind(struct tcp_slow_context *ctx,
    struct tcp_app_context_slow *actx, struct tcp_queue_entry *qe)
{
  int port_autobind, ip_autobind;
  __u8 is_port_valid;
  struct tcp_queue_entry *qe_res;
  struct tcp_queue_bind_req *req;
  struct tcp_queue_bind_res *res;
  struct tcp_sock *sock;

  qe_res = queue_tail(actx->slow_app_q);
  if (qe_res == NULL)
  {
    LOG_ERROR("failed to get tail of slow->app queue");
    return -1;
  }

  req = &qe->data.bind_req;
  res = &qe_res->data.bind_res;
  res->success = 0;
  res->opaque = req->opaque;

  is_port_valid = (req->local_port >= MIN_PORT && req->local_port <= MAX_PORT)
      || (req->local_port == 0);
  if (req->sock_id < ctx->n_socks && is_port_valid)
  {
    sock = &tcp_sock_map(ctx)[req->sock_id];
    
    port_autobind = 0;
    if (sock->local_port == 0)
      port_autobind = sock_autobind_port(ctx, sock);

    ip_autobind = 0;
    if (sock->local_ip == 0)
      ip_autobind = sock_autobind_ip(ctx, sock);
      
    if (port_autobind == 0 && ip_autobind == 0)
    {
      sock->local_ip = req->local_ip;
      sock->local_port = req->local_port;
      res->success = 1;
    }
  }

  if (queue_enqueue(actx->slow_app_q, TCP_QUEUE_BIND_RES) != 0)
  {
    LOG_ERROR("failed to enqueue bind response");
    return -1;
  }

  return res->success ? 0 : -1;
}

static int app_connect(struct tcp_slow_context *ctx,
    struct tcp_app_context_slow *actx, struct tcp_queue_entry *qe)
{
  int ret;
  struct tcp_queue_connect_req *req;
  struct tcp_sock *sock;

  req = &qe->data.connect_req;
  if (req->sock_id >= ctx->n_socks || req->remote_port == 0)
  {
    tcp_app_connect_res(actx, req->opaque, EINVAL, NULL);
    return -1;
  }

  sock = &tcp_sock_map(ctx)[req->sock_id];
  if (sock->state == TCP_SOCK_STATE_LISTEN)
  {
    tcp_app_connect_res(actx, sock->opaque, EOPNOTSUPP, sock);
    return -1;
  }

  if (sock->local_port == 0)
  {
    ret = sock_autobind_port(ctx, sock);
    if (ret != 0)
    {
      tcp_app_connect_res(actx, sock->opaque, EADDRINUSE, sock);
      return -1;
    }
  }
  
  if (sock->local_ip == 0)
  {
    ret = sock_autobind_ip(ctx, sock);
    if (ret != 0)
    {
      tcp_app_connect_res(actx, sock->opaque, EADDRINUSE, sock);
      return -1;
    }
  }

  LOG_DEBUG("app_connect: sock_id=%u remote_ip=%08x remote_port=%u local_ip=%08x local_port=%u",
      req->sock_id, req->remote_ip, req->remote_port, sock->local_ip, sock->local_port);

  return sock_connect_start(ctx, sock, req->remote_ip, req->remote_port);
}

static int app_listen(struct tcp_slow_context *ctx,
    struct tcp_app_context_slow *actx, struct tcp_queue_entry *qe)
{
  int ret;
  __u32 i;
  struct tcp_queue_listen_req *req;
  struct tcp_sock *sock;
  struct tcp_listener_slow *listener;

  req = &qe->data.listen_req;
  if (req->sock_id >= ctx->n_socks)
    return tcp_app_listen_res(actx, req->opaque, EBADF);

  sock = &tcp_sock_map(ctx)[req->sock_id];
  if (sock->local_port == 0)
    return tcp_app_listen_res(actx, req->opaque, EINVAL);

  ret = tcp_listener_insert(ctx, sock->local_port, sock->id, sock->reuport);
  if (ret != 0)
    return tcp_app_listen_res(actx, req->opaque, -ret);

  listener = &ctx->listeners[sock->id];
  if (!listener->active)
  {
    listener->ready_sids = calloc(req->backlog, sizeof(*listener->ready_sids));
    if (listener->ready_sids == NULL)
    {
      tcp_listener_remove(ctx, sock->local_port, sock->id);
      return tcp_app_listen_res(actx, req->opaque, ENOMEM);
    }

    for (i = 0; i < req->backlog; i++)
      listener->ready_sids[i] = ID_INVALID;
  }

  listener->active = 1;
  listener->sock_id = sock->id;
  listener->backlog_len = req->backlog;
  listener->backlog_used = 0;
  listener->ready_head = 0;
  listener->ready_used = 0;
  sock->state = TCP_SOCK_STATE_LISTEN;

  return tcp_app_listen_res(actx, req->opaque, 0);
}

static int app_accept(struct tcp_slow_context *ctx,
    struct tcp_app_context_slow *actx, struct tcp_queue_entry *qe)
{
  struct tcp_queue_accept_req *req;
  struct tcp_sock *listen_sock;
  struct tcp_sock *sock;
  struct tcp_listener_slow *listener;

  req = &qe->data.accept_req;
  if (req->sock_id >= ctx->n_socks)
    return app_accept_err(actx, NULL, req->opaque, EBADF);

  listen_sock = &tcp_sock_map(ctx)[req->sock_id];
  if (listen_sock->state != TCP_SOCK_STATE_LISTEN)
    return app_accept_err(actx, listen_sock, req->opaque, EINVAL);

  listener = &ctx->listeners[listen_sock->id];
  sock = tcp_listener_ready_pop(ctx, listen_sock->id);
  if (sock == NULL)
    return app_accept_err(actx, listen_sock, req->opaque, EAGAIN);

  sock_accept_own(ctx, sock, actx, req->opaque);
  if (listener->backlog_used != 0)
    listener->backlog_used--;

  return tcp_app_accept_res(actx, listen_sock, sock, 0);
}

static int app_shutdown(struct tcp_slow_context *ctx,
    struct tcp_app_context_slow *actx, struct tcp_queue_entry *qe)
{
  struct tcp_queue_shutdown_req *req;
  struct tcp_sock *sock;

  (void) actx;
  req = &qe->data.shutdown_req;
  if (req->sock_id >= ctx->n_socks)
    return -1;

  sock = &tcp_sock_map(ctx)[req->sock_id];
  if (req->how != SHUT_RDWR || sock->state != TCP_SOCK_STATE_ESTABLISHED)
    return -1;

  return sock_shutdown_start(ctx, sock);
}

static int app_close(struct tcp_slow_context *ctx,
    struct tcp_app_context_slow *actx, struct tcp_queue_entry *qe)
{
  struct tcp_queue_close_req *req;
  struct tcp_sock *sock;

  (void) actx;
  req = &qe->data.close_req;
  if (req->sock_id >= ctx->n_socks)
    return -1;

  sock = &tcp_sock_map(ctx)[req->sock_id];
  switch (sock->state)
  {
    case TCP_SOCK_STATE_LISTEN:
      sock_close_listen(ctx, sock);
      break;
    case TCP_SOCK_STATE_SYN_SENT:
    case TCP_SOCK_STATE_SYN_RECV:
    case TCP_SOCK_STATE_ACCEPT_PENDING:
      sock_abort_open(ctx, sock);
      break;
    case TCP_SOCK_STATE_ESTABLISHED:
      return sock_shutdown_start(ctx, sock);
    default:
      tcp_sock_close_final(ctx, sock);
      break;
  }

  return 0;
}

static int app_accept_err(struct tcp_app_context_slow *actx,
    struct tcp_sock *listen_sock, __u64 opaque, int err)
{
  struct tcp_sock listen_tmp = { .opaque = 0 };
  struct tcp_sock sock_tmp = { .opaque = opaque };

  if (listen_sock == NULL)
    listen_sock = &listen_tmp;

  return tcp_app_accept_res(actx, listen_sock, &sock_tmp, err);
}

/*** Socket Helpers ***********************************************************/

static int sock_autobind_port(struct tcp_slow_context *ctx, struct tcp_sock *sock)
{
  __u16 port;

  port = tcp_bound_find_free(ctx);
  if (port == 0)
    return -1;
  if (tcp_bound_insert(ctx, port, sock->id, 0) != 0)
    return -1;

  tcp_sock_meta(ctx, sock)->auto_bound = 1;
  sock->local_port = port;
  return 0;
}

static int sock_autobind_ip(struct tcp_slow_context *ctx, struct tcp_sock *sock)
{
  sock->local_ip = ctx->proto->local_ip;
  return 0;
}

static int sock_connect_start(struct tcp_slow_context *ctx, struct tcp_sock *sock,
    __u32 remote_ip, __u16 remote_port)
{
  __u16 flags;

  sock->remote_ip = remote_ip;
  sock->remote_port = remote_port;
  sock->tx_seq = 1;

  LOG_DEBUG("sock_connect_start: sock_id=%u remote_ip=%08x remote_port=%u local_ip=%08x local_port=%u",
      sock->id, sock->remote_ip, sock->remote_port, sock->local_ip, sock->local_port);
  sock->tx_pending = 1;
  sock->rx_seq = 0;
  sock->state = TCP_SOCK_STATE_SYN_SENT;
  if (tcp_cc_ecn_enabled(ctx))
    sock->flags |= TCP_SOCK_FLAG_ECN;
  else
    sock->flags &= ~TCP_SOCK_FLAG_ECN;

  if (tcp_flow_insert(ctx, sock) != 0)
  {
    tcp_sock_connect_fail(ctx, sock, ENOBUFS);
    return -1;
  }

  flags = TAS_TCP_SYN;
  if ((sock->flags & TCP_SOCK_FLAG_ECN) != 0)
    flags |= TAS_TCP_ECE | TAS_TCP_CWR;
  if (tcp_ctl_tx(ctx, sock, flags) != 0)
  {
    tcp_sock_connect_fail(ctx, sock, EAGAIN);
    return -1;
  }
  if (tcp_timeout_arm(ctx, sock, TCP_RETX_SYN) != 0)
  {
    tcp_sock_connect_fail(ctx, sock, ENOBUFS);
    return -1;
  }

  return 0;
}

static void sock_accept_own(struct tcp_slow_context *ctx, struct tcp_sock *sock,
    struct tcp_app_context_slow *actx, __u64 opaque)
{
  sock->opaque = opaque;
  sock->app_bump_qid = actx->app_bump_qs[sock->core]->id;
  sock->app_id = actx->app->id;
  sock->ctx_id = actx->id;
  tcp_sock_meta(ctx, sock)->listener_id = ID_INVALID;
  sock->state = TCP_SOCK_STATE_ESTABLISHED;
  tcp_sock_cc_init(ctx, sock);
}

static int sock_shutdown_start(struct tcp_slow_context *ctx, struct tcp_sock *sock)
{
  sock->flags |= TCP_SOCK_FLAG_SHUT_RD | TCP_SOCK_FLAG_SHUT_WR;
  sock->state = TCP_SOCK_STATE_FIN_WAIT1;
  if (tcp_ctl_tx(ctx, sock, TAS_TCP_FIN | TAS_TCP_ACK) != 0)
  {
    tcp_sock_close_final(ctx, sock);
    return -1;
  }
  if (tcp_timeout_arm(ctx, sock, TCP_RETX_FIN) != 0)
  {
    tcp_sock_close_final(ctx, sock);
    return -1;
  }

  return 0;
}

static void sock_abort_open(struct tcp_slow_context *ctx, struct tcp_sock *sock)
{
  if (sock->remote_port != 0)
  {
    tcp_ctl_tx(ctx, sock, sock->rx_seq == 0 ? TAS_TCP_RST :
        TAS_TCP_RST | TAS_TCP_ACK);
  }
  tcp_sock_close_final(ctx, sock);
}

static void sock_close_listen(struct tcp_slow_context *ctx, struct tcp_sock *sock)
{
  tcp_listener_remove(ctx, sock->local_port, sock->id);
  tcp_listener_abort_children(ctx, sock);
  tcp_listener_reset(ctx, sock->id);
  tcp_sock_close_final(ctx, sock);
}

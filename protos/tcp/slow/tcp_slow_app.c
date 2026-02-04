#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <errno.h>

#include "tcp_slow_internal.h"
#include "queue_fns.h"
#include "tcp_hdr.h"
#include "log.h"

static int handle_new_sock(struct tcp_slow_context *ctx,
    struct tcp_app_context_slow *actx, struct tcp_queue_entry *qe_req);
static int handle_bind(struct tcp_slow_context *ctx,
    struct tcp_app_context_slow *actx, struct tcp_queue_entry *qe_req);
static int handle_sock_setopt(struct tcp_slow_context *ctx,
    struct tcp_app_context_slow *actx, struct tcp_queue_entry *qe_req);
static int handle_connect_req(struct tcp_slow_context *ctx,
    struct tcp_app_context_slow *actx, struct tcp_queue_entry *qe_req);
static int handle_listen_req(struct tcp_slow_context *ctx,
    struct tcp_app_context_slow *actx, struct tcp_queue_entry *qe_req);
static int handle_accept_req(struct tcp_slow_context *ctx,
    struct tcp_app_context_slow *actx, struct tcp_queue_entry *qe_req);
static int handle_shutdown_req(struct tcp_slow_context *ctx,
    struct tcp_app_context_slow *actx, struct tcp_queue_entry *qe_req);
static int handle_close_req(struct tcp_slow_context *ctx,
    struct tcp_app_context_slow *actx, struct tcp_queue_entry *qe_req);

int tcp_slow_app_poll(struct tcp_slow_context *ctx)
{
  int msgs_i, apps_polled, ctxs_polled;
  __u8 type;
  struct dqueue *q;
  struct tcp_queue_entry *qe;
  struct tcp_app_slow *a;
  struct tcp_app_context_slow *actx;

  msgs_i = 0;
  apps_polled = 0;
  ctxs_polled = 0;
  while (msgs_i < SLOW_BATCH_SIZE && ctx->n_apps != 0)
  {
    a = &ctx->apps[ctx->next_app];
    if (ctxs_polled >= a->n_ctxs)
    {
      apps_polled++;
      ctx->next_app = (ctx->next_app + 1) % ctx->n_apps;
      a = &ctx->apps[ctx->next_app];
    }

    if (apps_polled >= ctx->n_apps || a->n_ctxs == 0)
      break;

    actx = &a->ctxs[a->next_ctx];
    q = actx->app_slow_q;
    qe = queue_head(q);

    if (qe == NULL)
    {
      ctxs_polled++;
      a->next_ctx = (a->next_ctx + 1) % a->n_ctxs;
      continue;
    }

    msgs_i++;
    type = qe->type;
    switch (type)
    {
      case TCP_QUEUE_EMPTY:
        break;
      case TCP_QUEUE_NEW_SOCK_REQ:
        handle_new_sock(ctx, actx, qe);
        break;
      case TCP_QUEUE_SETOPT_REQ:
        handle_sock_setopt(ctx, actx, qe);
        break;
      case TCP_QUEUE_BIND_REQ:
        handle_bind(ctx, actx, qe);
        break;
      case TCP_QUEUE_CONNECT_REQ:
        handle_connect_req(ctx, actx, qe);
        break;
      case TCP_QUEUE_LISTEN_REQ:
        handle_listen_req(ctx, actx, qe);
        break;
      case TCP_QUEUE_ACCEPT_REQ:
        handle_accept_req(ctx, actx, qe);
        break;
      case TCP_QUEUE_SHUTDOWN_REQ:
        handle_shutdown_req(ctx, actx, qe);
        break;
      case TCP_QUEUE_CLOSE_REQ:
        handle_close_req(ctx, actx, qe);
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

int tcp_slow_app_enqueue_listen_res(struct tcp_app_context_slow *actx,
    __u64 opaque, int status)
{
  int ret;
  struct tcp_queue_entry *qe;

  qe = queue_tail(actx->slow_app_q);
  if (qe == NULL)
  {
    LOG_ERROR("failed to get tail of slow->app queue");
    return -1;
  }

  qe->data.listen_res.status = status;
  qe->data.listen_res.opaque = opaque;

  ret = queue_enqueue(actx->slow_app_q, TCP_QUEUE_LISTEN_RES);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue listen response");
    return -1;
  }

  return 0;
}

int tcp_slow_app_enqueue_connect_res(struct tcp_app_context_slow *actx,
    __u64 opaque, int status, struct tcp_sock *sock)
{
  int ret;
  struct tcp_queue_entry *qe;
  struct tcp_queue_connect_res *res;

  qe = queue_tail(actx->slow_app_q);
  if (qe == NULL)
  {
    LOG_ERROR("failed to get tail of slow->app queue");
    return -1;
  }

  res = &qe->data.connect_res;
  res->status = status;
  res->opaque = opaque;
  if (sock == NULL)
  {
    res->local_ip = 0;
    res->local_port = 0;
    res->remote_ip = 0;
    res->remote_port = 0;
  }
  else
  {
    res->local_ip = sock->local_ip;
    res->local_port = sock->local_port;
    res->remote_ip = sock->remote_ip;
    res->remote_port = sock->remote_port;
  }

  ret = queue_enqueue(actx->slow_app_q, TCP_QUEUE_CONNECT_RES);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue connect response");
    return -1;
  }

  return 0;
}

int tcp_slow_app_enqueue_accept_res(struct tcp_app_context_slow *actx,
    struct tcp_sock *listen_sock, struct tcp_sock *sock, int status)
{
  int ret;
  struct tcp_queue_entry *qe;
  struct tcp_queue_accept_res *res;

  qe = queue_tail(actx->slow_app_q);
  if (qe == NULL)
  {
    LOG_ERROR("failed to get tail of slow->app queue");
    return -1;
  }

  res = &qe->data.accept_res;
  memset(res, 0, sizeof(*res));
  res->status = status;
  res->opaque = sock->opaque;
  res->listen_opaque = listen_sock->opaque;
  if (status == 0)
    tcp_slow_state_fill_accept_res(listen_sock, sock, res);

  ret = queue_enqueue(actx->slow_app_q, TCP_QUEUE_ACCEPT_RES);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue accept response");
    return -1;
  }

  return 0;
}

int tcp_slow_app_enqueue_listen_newconn(struct tcp_slow_context *ctx,
    struct tcp_sock *listen_sock, struct tcp_sock *sock)
{
  int ret;
  struct tcp_app_context_slow *actx;
  struct tcp_queue_entry *qe;

  actx = tcp_slow_sock_actx(ctx, listen_sock);
  qe = queue_tail(actx->slow_app_q);
  if (qe == NULL)
  {
    LOG_ERROR("failed to get tail of slow->app queue");
    return -1;
  }

  qe->data.listen_newconn.opaque = listen_sock->opaque;
  qe->data.listen_newconn.remote_ip = sock->remote_ip;
  qe->data.listen_newconn.remote_port = sock->remote_port;

  ret = queue_enqueue(actx->slow_app_q, TCP_QUEUE_LISTEN_NEWCONN);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue listen-newconn response");
    return -1;
  }

  return 0;
}

static int handle_new_sock(struct tcp_slow_context *ctx,
    struct tcp_app_context_slow *actx, struct tcp_queue_entry *qe_req)
{
  int ret;
  struct tcp_sock *sock;
  struct tcp_queue_entry *qe_res;
  struct tcp_queue_new_sock_req *req;
  struct tcp_queue_new_sock_res *res;

  req = &qe_req->data.new_sock_req;
  ret = tcp_slow_state_alloc_sock(ctx, req->opaque, actx->app->id, actx->id,
      actx->app_bump_qs[0]->id, &sock);
  if (ret != 0)
    return -1;

  qe_res = queue_tail(actx->slow_app_q);
  if (qe_res == NULL)
  {
    LOG_ERROR("failed to get tail of slow->app queue");
    return -1;
  }

  res = &qe_res->data.new_sock_res;
  tcp_slow_state_fill_new_sock_res(sock, res);

  ret = queue_enqueue(actx->slow_app_q, TCP_QUEUE_NEW_SOCK_RES);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue new socket response");
    return -1;
  }

  return 0;
}

static int handle_sock_setopt(struct tcp_slow_context *ctx,
    struct tcp_app_context_slow *actx, struct tcp_queue_entry *qe_req)
{
  int ret;
  struct tcp_sock *sock;
  struct tcp_queue_entry *qe_res;
  struct tcp_queue_setopt_req *req;
  struct tcp_queue_setopt_res *res;

  req = &qe_req->data.setopt_req;
  qe_res = queue_tail(actx->slow_app_q);
  if (qe_res == NULL)
  {
    LOG_ERROR("failed to get tail of slow->app queue");
    return -1;
  }
  res = &qe_res->data.setopt_res;

  if (req->sock_id >= ctx->n_socks)
  {
    res->success = 0;
    res->opaque = req->opaque;
    queue_enqueue(actx->slow_app_q, TCP_QUEUE_SETOPT_RES);
    return -1;
  }

  sock = &tcp_slow_get_sock_map(ctx)[req->sock_id];
  switch (req->opt)
  {
    case SO_REUSEPORT:
      sock->reuport = 1;
      break;
    default:
      res->success = 0;
      res->opaque = req->opaque;
      queue_enqueue(actx->slow_app_q, TCP_QUEUE_SETOPT_RES);
      return -1;
  }

  res->success = 1;
  res->opaque = req->opaque;
  ret = queue_enqueue(actx->slow_app_q, TCP_QUEUE_SETOPT_RES);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue TCP queue setopt response");
    return -1;
  }

  return 0;
}

static int handle_bind(struct tcp_slow_context *ctx,
    struct tcp_app_context_slow *actx, struct tcp_queue_entry *qe_req)
{
  int ret;
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

  req = &qe_req->data.bind_req;
  res = &qe_res->data.bind_res;
  res->success = 0;
  res->opaque = req->opaque;

  if (req->sock_id >= ctx->n_socks || req->local_port < MIN_PORT ||
      req->local_port > MAX_PORT)
    goto out;

  sock = &tcp_slow_get_sock_map(ctx)[req->sock_id];
  if (sock->local_port != 0)
    goto out;

  ret = tcp_slow_state_bound_insert(ctx, req->local_port, req->sock_id,
      sock->reuport);
  if (ret != 0)
    goto out;

  sock->local_ip = req->local_ip;
  sock->local_port = req->local_port;
  res->success = 1;

out:
  ret = queue_enqueue(actx->slow_app_q, TCP_QUEUE_BIND_RES);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue TCP queue bind response");
    return -1;
  }

  return 0;
}

static int handle_connect_req(struct tcp_slow_context *ctx,
    struct tcp_app_context_slow *actx, struct tcp_queue_entry *qe_req)
{
  int ret;
  __u16 port;
  struct tcp_queue_connect_req *req;
  struct tcp_sock *sock;

  req = &qe_req->data.connect_req;
  if (req->sock_id >= ctx->n_socks || req->remote_port == 0)
  {
    tcp_slow_app_enqueue_connect_res(actx, req->opaque, EINVAL, NULL);
    return -1;
  }

  sock = &tcp_slow_get_sock_map(ctx)[req->sock_id];
  if (sock->state == TCP_SOCK_STATE_LISTEN)
  {
    tcp_slow_app_enqueue_connect_res(actx, sock->opaque, EOPNOTSUPP, sock);
    return -1;
  }

  if (sock->local_port == 0)
  {
    port = tcp_slow_state_bound_find_free_port(ctx);
    if (port == 0)
    {
      tcp_slow_app_enqueue_connect_res(actx, sock->opaque, EADDRINUSE, sock);
      return -1;
    }

    ret = tcp_slow_state_bound_insert(ctx, port, sock->id, 0);
    if (ret != 0)
    {
      tcp_slow_app_enqueue_connect_res(actx, sock->opaque, EADDRINUSE, sock);
      return -1;
    }

    ctx->sock_meta[sock->id].auto_bound = 1;
    sock->local_ip = ctx->proto->local_ip;
    sock->local_port = port;
  }

  sock->remote_ip = req->remote_ip;
  sock->remote_port = req->remote_port;
  sock->tx_seq = 1;
  sock->rx_seq = 0;
  sock->state = TCP_SOCK_STATE_SYN_SENT;

  ret = tcp_slow_state_flow_insert(ctx, sock);
  if (ret != 0)
  {
    tcp_slow_state_sock_connect_failed(ctx, sock, ENOBUFS);
    return -1;
  }

  ret = tcp_slow_state_enqueue_ctrl_tx(ctx, sock, TAS_TCP_SYN);
  if (ret != 0)
  {
    tcp_slow_state_sock_connect_failed(ctx, sock, EAGAIN);
    return -1;
  }

  ret = tcp_slow_timeout_arm(ctx, sock, TCP_SLOW_RETX_SYN);
  if (ret != 0)
  {
    tcp_slow_state_sock_connect_failed(ctx, sock, ENOBUFS);
    return -1;
  }

  return 0;
}

static int handle_listen_req(struct tcp_slow_context *ctx,
    struct tcp_app_context_slow *actx, struct tcp_queue_entry *qe_req)
{
  int ret;
  __u32 sock_id;
  struct tcp_queue_listen_req *req;
  struct tcp_sock *sock;
  struct tcp_listener_slow *listener;

  req = &qe_req->data.listen_req;
  sock_id = req->sock_id;
  if (sock_id >= ctx->n_socks)
    return tcp_slow_app_enqueue_listen_res(actx, req->opaque, EBADF);

  sock = &tcp_slow_get_sock_map(ctx)[sock_id];
  if (sock->local_port == 0)
    return tcp_slow_app_enqueue_listen_res(actx, req->opaque, EINVAL);

  ret = tcp_slow_state_listener_insert(ctx, sock->local_port, sock->id,
      sock->reuport);
  if (ret != 0)
    return tcp_slow_app_enqueue_listen_res(actx, req->opaque, -ret);

  listener = &ctx->listeners[sock->id];
  if (!listener->active)
  {
    listener->ready_sids = calloc(req->backlog, sizeof(*listener->ready_sids));
    if (listener->ready_sids == NULL)
    {
      tcp_slow_state_listener_remove(ctx, sock->local_port, sock->id);
      return tcp_slow_app_enqueue_listen_res(actx, req->opaque, ENOMEM);
    }
    for (__u32 i = 0; i < req->backlog; i++)
      listener->ready_sids[i] = ID_INVALID;
  }

  listener->active = 1;
  listener->sock_id = sock->id;
  listener->backlog_len = req->backlog;
  listener->backlog_used = 0;
  listener->ready_head = 0;
  listener->ready_used = 0;
  sock->state = TCP_SOCK_STATE_LISTEN;

  return tcp_slow_app_enqueue_listen_res(actx, req->opaque, 0);
}

static int handle_accept_req(struct tcp_slow_context *ctx,
    struct tcp_app_context_slow *actx, struct tcp_queue_entry *qe_req)
{
  int ret;
  struct tcp_queue_accept_req *req;
  struct tcp_sock *listen_sock;
  struct tcp_sock *sock;
  struct tcp_listener_slow *listener;

  req = &qe_req->data.accept_req;
  if (req->sock_id >= ctx->n_socks)
  {
    struct tcp_sock tmp = { .opaque = req->opaque };
    struct tcp_sock listen_tmp = { .opaque = 0 };
    tcp_slow_app_enqueue_accept_res(actx, &listen_tmp, &tmp, EBADF);
    return -1;
  }

  listen_sock = &tcp_slow_get_sock_map(ctx)[req->sock_id];
  if (listen_sock->state != TCP_SOCK_STATE_LISTEN)
  {
    struct tcp_sock tmp = { .opaque = req->opaque };
    tcp_slow_app_enqueue_accept_res(actx, listen_sock, &tmp, EINVAL);
    return -1;
  }

  listener = &ctx->listeners[listen_sock->id];
  sock = tcp_slow_state_listener_ready_pop(ctx, listen_sock->id);
  if (sock == NULL)
  {
    struct tcp_sock tmp = { .opaque = req->opaque };
    tcp_slow_app_enqueue_accept_res(actx, listen_sock, &tmp, EAGAIN);
    return -1;
  }

  sock->opaque = req->opaque;
  sock->app_bump_qid = actx->app_bump_qs[sock->core]->id;
  sock->app_id = actx->app->id;
  sock->ctx_id = actx->id;
  ctx->sock_meta[sock->id].listener_id = ID_INVALID;
  sock->state = TCP_SOCK_STATE_ESTABLISHED;
  if (listener->backlog_used != 0)
    listener->backlog_used--;

  ret = tcp_slow_app_enqueue_accept_res(actx, listen_sock, sock, 0);
  if (ret != 0)
    return -1;

  return 0;
}

static int handle_shutdown_req(struct tcp_slow_context *ctx,
    struct tcp_app_context_slow *actx, struct tcp_queue_entry *qe_req)
{
  struct tcp_queue_shutdown_req *req;
  struct tcp_sock *sock;

  (void) actx;
  req = &qe_req->data.shutdown_req;
  if (req->sock_id >= ctx->n_socks)
    return -1;

  sock = &tcp_slow_get_sock_map(ctx)[req->sock_id];
  if (req->how != SHUT_RDWR || sock->state != TCP_SOCK_STATE_ESTABLISHED)
    return -1;

  sock->flags |= TCP_SOCK_FLAG_SHUT_RD | TCP_SOCK_FLAG_SHUT_WR;
  sock->state = TCP_SOCK_STATE_FIN_WAIT1;
  if (tcp_slow_state_enqueue_ctrl_tx(ctx, sock, TAS_TCP_FIN | TAS_TCP_ACK) != 0)
  {
    tcp_slow_state_sock_close_final(ctx, sock);
    return -1;
  }
  if (tcp_slow_timeout_arm(ctx, sock, TCP_SLOW_RETX_FIN) != 0)
  {
    tcp_slow_state_sock_close_final(ctx, sock);
    return -1;
  }
  return 0;
}

static int handle_close_req(struct tcp_slow_context *ctx,
    struct tcp_app_context_slow *actx, struct tcp_queue_entry *qe_req)
{
  struct tcp_queue_close_req *req;
  struct tcp_sock *sock;

  (void) actx;
  req = &qe_req->data.close_req;
  if (req->sock_id >= ctx->n_socks)
    return -1;

  sock = &tcp_slow_get_sock_map(ctx)[req->sock_id];
  switch (sock->state)
  {
    case TCP_SOCK_STATE_LISTEN:
      tcp_slow_state_listener_remove(ctx, sock->local_port, sock->id);
      tcp_slow_state_listener_abort_children(ctx, sock);
      tcp_slow_state_listener_reset(ctx, sock->id);
      tcp_slow_state_sock_close_final(ctx, sock);
      break;
    case TCP_SOCK_STATE_SYN_SENT:
    case TCP_SOCK_STATE_SYN_RECV:
    case TCP_SOCK_STATE_ACCEPT_PENDING:
      if (sock->remote_port != 0)
      {
        tcp_slow_state_enqueue_ctrl_tx(ctx, sock, sock->rx_seq == 0 ?
            TAS_TCP_RST : TAS_TCP_RST | TAS_TCP_ACK);
      }
      tcp_slow_state_sock_close_final(ctx, sock);
      break;
    case TCP_SOCK_STATE_ESTABLISHED:
      sock->flags |= TCP_SOCK_FLAG_SHUT_RD | TCP_SOCK_FLAG_SHUT_WR;
      sock->state = TCP_SOCK_STATE_FIN_WAIT1;
      if (tcp_slow_state_enqueue_ctrl_tx(ctx, sock,
              TAS_TCP_FIN | TAS_TCP_ACK) != 0 ||
          tcp_slow_timeout_arm(ctx, sock, TCP_SLOW_RETX_FIN) != 0)
      {
        tcp_slow_state_sock_close_final(ctx, sock);
      }
      break;
    default:
      tcp_slow_state_sock_close_final(ctx, sock);
      break;
  }

  return 0;
}

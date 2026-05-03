#include <string.h>

#include "tcp_internal.h"
#include "queue_fns.h"
#include "log.h"

/*** Response Helpers *********************************************************/

static struct tcp_queue_entry *app_reserve(struct tcp_app_context_slow *actx);
static int app_submit(struct tcp_app_context_slow *actx, __u8 type,
    const char *what);

/*** Response API *************************************************************/

int tcp_app_listen_res(struct tcp_app_context_slow *actx, __u64 opaque,
    int status)
{
  struct tcp_queue_entry *qe;

  qe = app_reserve(actx);
  if (qe == NULL)
    return -1;

  qe->data.listen_res.status = status;
  qe->data.listen_res.opaque = opaque;
  return app_submit(actx, TCP_QUEUE_LISTEN_RES, "listen response");
}

int tcp_app_connect_res(struct tcp_app_context_slow *actx, __u64 opaque,
    int status, struct tcp_sock *sock)
{
  struct tcp_queue_entry *qe;
  struct tcp_queue_connect_res *res;

  qe = app_reserve(actx);
  if (qe == NULL)
    return -1;

  res = &qe->data.connect_res;
  res->status = status;
  res->opaque = opaque;
  if (sock == NULL)
  {
    res->core = 0;
    res->local_ip = 0;
    res->local_port = 0;
    res->remote_ip = 0;
    res->remote_port = 0;
  }
  else
  {
    res->core = sock->core;
    res->local_ip = sock->local_ip;
    res->local_port = sock->local_port;
    res->remote_ip = sock->remote_ip;
    res->remote_port = sock->remote_port;
  }

  return app_submit(actx, TCP_QUEUE_CONNECT_RES, "connect response");
}

int tcp_app_accept_res(struct tcp_app_context_slow *actx,
    struct tcp_sock *listen_sock, struct tcp_sock *sock, int status)
{
  struct tcp_queue_entry *qe;
  struct tcp_queue_accept_res *res;

  qe = app_reserve(actx);
  if (qe == NULL)
    return -1;

  res = &qe->data.accept_res;
  memset(res, 0, sizeof(*res));
  res->status = status;
  res->opaque = sock->opaque;
  res->listen_opaque = listen_sock->opaque;
  if (status == 0)
    tcp_sock_accept_res_fill(listen_sock, sock, res);

  return app_submit(actx, TCP_QUEUE_ACCEPT_RES, "accept response");
}

int tcp_app_listen_newconn(struct tcp_slow_context *ctx,
    struct tcp_sock *listen_sock, struct tcp_sock *sock)
{
  struct tcp_app_context_slow *actx;
  struct tcp_queue_entry *qe;

  actx = tcp_sock_actx(ctx, listen_sock);
  qe = app_reserve(actx);
  if (qe == NULL)
    return -1;

  qe->data.listen_newconn.opaque = listen_sock->opaque;
  qe->data.listen_newconn.remote_ip = sock->remote_ip;
  qe->data.listen_newconn.remote_port = sock->remote_port;
  return app_submit(actx, TCP_QUEUE_LISTEN_NEWCONN, "listen-newconn response");
}

/*** Response Helpers *********************************************************/

static struct tcp_queue_entry *app_reserve(struct tcp_app_context_slow *actx)
{
  struct tcp_queue_entry *qe;

  qe = queue_tail(actx->slow_app_q);
  if (qe == NULL)
    LOG_ERROR("failed to get tail of slow->app queue");

  return qe;
}

static int app_submit(struct tcp_app_context_slow *actx, __u8 type,
    const char *what)
{
  if (queue_enqueue(actx->slow_app_q, type) != 0)
  {
    LOG_ERROR("failed to enqueue %s", what);
    return -1;
  }

  return 0;
}

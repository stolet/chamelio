#include <stdlib.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/un.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <cham_lib.h>

#include "log.h"
#include "tcp_appif.h"
#include "tcp_slow.h"
#include "queue_fns.h"
#include "uxsocket.h"
#include "internal.h"

/*** Enums ********************************************************************/

enum tcp_appif_ev_type {
  TCP_APPIF_EV_LISTEN = 1,
  TCP_APPIF_EV_CONN,
};

/*** Uxsocket Helpers *********************************************************/

static struct tcp_appif_event *tcp_appif_event_new(int type, int fd,
    struct tcp_app_slow *app);
static int tcp_appif_epoll_add(struct tcp_slow_context *ctx, int fd, __u32 events,
    struct tcp_appif_event *aev);
static void tcp_appif_event_close(struct tcp_slow_context *ctx,
    struct tcp_appif_event *aev);
static int tcp_appif_epoll_open(struct tcp_slow_context *ctx);
static int tcp_appif_listen_open(struct tcp_slow_context *ctx);
static int tcp_appif_req_recv(struct tcp_appif_event *aev);
static struct proto_queue_lib *tcp_appif_queue_new(struct tcp_slow_context *ctx,
    __u32 len, __u32 elsize, const char *what);
static int tcp_appif_app_qs_init(struct tcp_slow_context *ctx,
    struct tcp_appif_event *aev, struct tcp_app_context_slow *actx);
static int tcp_appif_bump_qs_init(struct tcp_slow_context *ctx,
    struct tcp_appif_event *aev, struct tcp_app_context_slow *actx);
static int tcp_appif_actx_init(struct tcp_slow_context *ctx,
    struct tcp_appif_event *aev);
static int tcp_appif_new_actx_res_send(struct tcp_appif_event *aev);
static int tcp_appif_conn_rx(struct tcp_slow_context *ctx,
    struct tcp_appif_event *aev);
static int tcp_appif_listen_accept(struct tcp_slow_context *ctx);
static int tcp_appif_poll_ev(struct tcp_slow_context *ctx,
    const struct epoll_event *ev);

/*** Public API ***************************************************************/

int tcp_appif_init(struct tcp_slow_context *ctx)
{
  if (tcp_appif_epoll_open(ctx) != 0)
  {
    LOG_ERROR("appif epoll init failed");
    return -1;
  }
  if (tcp_appif_listen_open(ctx) != 0)
  {
    LOG_ERROR("failed to init unix socket for apps");
    close(ctx->app_epfd);
    ctx->app_epfd = -1;
    return -1;
  }

  return 0;
}

int tcp_appif_poll(struct tcp_slow_context *ctx)
{
  int i;
  int nr;
  struct epoll_event evs[32];

  nr = epoll_wait(ctx->app_epfd, evs, 32, 0);
  for (i = 0; i < nr; i++)
    tcp_appif_poll_ev(ctx, &evs[i]);

  return nr;
}

/*** Uxsocket Helpers *********************************************************/

static struct tcp_appif_event *tcp_appif_event_new(int type, int fd,
    struct tcp_app_slow *app)
{
  struct tcp_appif_event *aev;

  aev = malloc(sizeof(*aev));
  if (aev == NULL)
    return NULL;

  memset(aev, 0, sizeof(*aev));
  aev->type = type;
  aev->fd = fd;
  aev->app = app;
  return aev;
}

static int tcp_appif_epoll_add(struct tcp_slow_context *ctx, int fd, __u32 events,
    struct tcp_appif_event *aev)
{
  struct epoll_event ev;

  memset(&ev, 0, sizeof(ev));
  ev.events = events;
  ev.data.ptr = aev;
  return epoll_ctl(ctx->app_epfd, EPOLL_CTL_ADD, fd, &ev);
}

static void tcp_appif_event_close(struct tcp_slow_context *ctx,
    struct tcp_appif_event *aev)
{
  epoll_ctl(ctx->app_epfd, EPOLL_CTL_DEL, aev->fd, NULL);
  close(aev->fd);
  free(aev);
}

static int tcp_appif_epoll_open(struct tcp_slow_context *ctx)
{
  ctx->app_epfd = epoll_create1(0);
  if (ctx->app_epfd == -1)
  {
    LOG_ERROR("epoll_create failed");
    perror("");
    return -1;
  }

  return 0;
}

static int tcp_appif_listen_open(struct tcp_slow_context *ctx)
{
  int fd;
  struct sockaddr_un saun;
  struct tcp_appif_event *aev;

  fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd == -1)
  {
    LOG_ERROR("socket creation failed");
    perror("");
    return -1;
  }

  memset(&saun, 0, sizeof(saun));
  saun.sun_family = AF_UNIX;
  memcpy(saun.sun_path, TCP_APP_SOCKET_PATH, sizeof(TCP_APP_SOCKET_PATH));
  unlink(saun.sun_path);

  if (bind(fd, (struct sockaddr *) &saun, sizeof(saun)) != 0)
  {
    LOG_ERROR("bind failed");
    perror("");
    goto err_close;
  }
  if (listen(fd, 5) != 0)
  {
    LOG_ERROR("listen failed");
    perror("");
    goto err_close;
  }

  aev = tcp_appif_event_new(TCP_APPIF_EV_LISTEN, -1, NULL);
  if (aev == NULL)
  {
    LOG_ERROR("failed to allocate appif listen event");
    goto err_close;
  }
  if (tcp_appif_epoll_add(ctx, fd, EPOLLIN, aev) != 0)
  {
    LOG_ERROR("epoll_ctl listen failed");
    perror("");
    free(aev);
    goto err_close;
  }

  ctx->app_uxfd = fd;
  return 0;

err_close:
  close(fd);
  return -1;
}

static int tcp_appif_req_recv(struct tcp_appif_event *aev)
{
  ssize_t n;
  struct iovec iov = {
    .iov_base = &aev->app_req,
    .iov_len = sizeof(aev->app_req) - aev->req_rx,
  };
  union {
    char buf[CMSG_SPACE(sizeof(int))];
    struct cmsghdr align;
  } u;
  struct msghdr msg = {
    .msg_name = NULL,
    .msg_namelen = 0,
    .msg_iov = &iov,
    .msg_iovlen = 1,
    .msg_control = u.buf,
    .msg_controllen = sizeof(u.buf),
    .msg_flags = 0,
  };

  n = recvmsg(aev->fd, &msg, 0);
  if (n < 0)
  {
    LOG_ERROR("failed to recvmsg");
    perror("");
    return -1;
  }

  if (msg.msg_controllen > 0)
  {
    struct cmsghdr *cmsg;

    cmsg = CMSG_FIRSTHDR(&msg);
    assert(cmsg != NULL);
    assert(cmsg->cmsg_len == CMSG_LEN(sizeof(int)));
  }

  if ((size_t) n + aev->req_rx < sizeof(aev->app_req))
  {
    aev->req_rx += n;
    return 0;
  }

  aev->req_rx = 0;
  return 1;
}

static struct proto_queue_lib *tcp_appif_queue_new(struct tcp_slow_context *ctx,
    __u32 len, __u32 elsize, const char *what)
{
  struct proto_queue_lib *protoq;

  protoq = cham_new_queue(ctx->proto, len, elsize);
  if (protoq == NULL)
    LOG_ERROR("failed to create %s", what);

  return protoq;
}

static int tcp_appif_app_qs_init(struct tcp_slow_context *ctx,
    struct tcp_appif_event *aev, struct tcp_app_context_slow *actx)
{
  struct dqueue *dq;
  struct equeue *eq;
  struct proto_queue_lib *protoq;

  protoq = tcp_appif_queue_new(ctx, ctx->config.appq_len,
      sizeof(struct tcp_queue_entry), "queue app->slow");
  if (protoq == NULL)
    return -1;

  dq = dqueue_new(protoq->nelems, protoq->elsize,
      ctx->proto->shm_base + protoq->off, protoq->off);
  if (dq == NULL)
  {
    LOG_ERROR("failed to create dqueue for app->slow");
    return -1;
  }

  aev->app_res.as_nelems = protoq->nelems;
  aev->app_res.as_elsize = protoq->elsize;
  aev->app_res.as_off = protoq->off;
  actx->app_slow_q = dq;

  protoq = tcp_appif_queue_new(ctx, ctx->config.appq_len,
      sizeof(struct tcp_queue_entry), "queue slow->app");
  if (protoq == NULL)
    return -1;

  eq = equeue_new(protoq->nelems, protoq->elsize,
      ctx->proto->shm_base + protoq->off, protoq->off);
  if (eq == NULL)
  {
    LOG_ERROR("failed to create equeue for slow->app");
    return -1;
  }

  aev->app_res.sa_nelems = protoq->nelems;
  aev->app_res.sa_elsize = protoq->elsize;
  aev->app_res.sa_off = protoq->off;
  actx->slow_app_q = eq;
  return 0;
}

static int tcp_appif_bump_qs_init(struct tcp_slow_context *ctx,
    struct tcp_appif_event *aev, struct tcp_app_context_slow *actx)
{
  int i;
  struct proto_queue_lib *protoq;

  for (i = 0; i < ctx->proto->n_fp_cores; i++)
  {
    protoq = tcp_appif_queue_new(ctx, ctx->config.bumpq_len,
        sizeof(struct tcp_queue_bump_entry), "queue fast->app");
    if (protoq == NULL)
      return -1;

    aev->app_res.fa_nelems = protoq->nelems;
    aev->app_res.fa_elsize = protoq->elsize;
    aev->app_res.fa_offs[i] = protoq->off;
    actx->app_bump_qs[i] = protoq;

    protoq = tcp_appif_queue_new(ctx, ctx->config.bumpq_len,
        sizeof(struct tcp_queue_bump_entry), "queue app->fast");
    if (protoq == NULL)
      return -1;

    aev->app_res.af_nelems = protoq->nelems;
    aev->app_res.af_elsize = protoq->elsize;
    aev->app_res.af_offs[i] = protoq->off;
    actx->fast_bump_qs[i] = protoq;
    cham_enable_queue(ctx->proto, protoq->id, 0);
  }

  return 0;
}

static int tcp_appif_actx_init(struct tcp_slow_context *ctx,
    struct tcp_appif_event *aev)
{
  struct tcp_app_context_slow *actx;

  actx = &aev->app->ctxs[aev->app->n_ctxs];
  if (tcp_appif_app_qs_init(ctx, aev, actx) != 0)
    return -1;
  if (tcp_appif_bump_qs_init(ctx, aev, actx) != 0)
    return -1;

  actx->id = aev->app->n_ctxs;
  actx->app = aev->app;
  aev->app->n_ctxs++;
  aev->app_res.n_fp_cores = ctx->proto->n_fp_cores;
  aev->app_res.shm_len = ctx->proto->shm_size;
  aev->app_res.shm_off = ctx->proto->shm_off;
  return 0;
}

static int tcp_appif_new_actx_res_send(struct tcp_appif_event *aev)
{
  size_t len;
  ssize_t n;

  len = sizeof(aev->app_res);
  n = send(aev->fd, &aev->app_res, len, 0);
  if (n < 0)
  {
    LOG_ERROR("send failed");
    perror("");
    return -1;
  }
  if ((size_t) n < len)
  {
    LOG_ERROR("short send for response");
    return -1;
  }

  return 0;
}

static int tcp_appif_conn_rx(struct tcp_slow_context *ctx,
    struct tcp_appif_event *aev)
{
  int ret;

  ret = tcp_appif_req_recv(aev);
  if (ret <= 0)
    return ret;

  if (tcp_appif_actx_init(ctx, aev) != 0)
    return -1;
  if (tcp_appif_new_actx_res_send(aev) != 0)
    return -1;

  return 0;
}

static int tcp_appif_listen_accept(struct tcp_slow_context *ctx)
{
  int fd;
  struct tcp_app_slow *app;
  struct tcp_appif_event *aev;

  fd = accept(ctx->app_uxfd, NULL, NULL);
  if (fd < 0)
  {
    LOG_ERROR("accept failed");
    perror("");
    return -1;
  }
  if (uxsocket_sendfd(fd, ctx->proto->shm_fd, -1) < 0)
  {
    LOG_ERROR("failed to send shm fd");
    close(fd);
    return -1;
  }

  app = &ctx->apps[ctx->n_apps];
  app->id = ctx->n_apps;
  app->n_ctxs = 0;
  app->next_ctx = 0;

  aev = tcp_appif_event_new(TCP_APPIF_EV_CONN, fd, app);
  if (aev == NULL)
  {
    LOG_ERROR("failed to allocate appif connection event");
    close(fd);
    return -1;
  }
  if (tcp_appif_epoll_add(ctx, fd, EPOLLIN | EPOLLRDHUP | EPOLLERR, aev) != 0)
  {
    LOG_ERROR("epoll_ctl failed");
    perror("");
    free(aev);
    close(fd);
    return -1;
  }

  ctx->n_apps++;
  return 0;
}

static int tcp_appif_poll_ev(struct tcp_slow_context *ctx,
    const struct epoll_event *ev)
{
  struct tcp_appif_event *aev;

  aev = ev->data.ptr;
  switch (aev->type)
  {
    case TCP_APPIF_EV_LISTEN:
      return tcp_appif_listen_accept(ctx);
    case TCP_APPIF_EV_CONN:
      if ((ev->events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) != 0)
      {
        tcp_appif_event_close(ctx, aev);
        return 0;
      }
      if ((ev->events & EPOLLIN) != 0 && tcp_appif_conn_rx(ctx, aev) != 0)
      {
        tcp_appif_event_close(ctx, aev);
        return -1;
      }
      return 0;
    default:
      LOG_WARN("unknown app event type");
      return -1;
  }
}

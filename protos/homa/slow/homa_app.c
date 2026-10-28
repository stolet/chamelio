#define _GNU_SOURCE

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "homa_slow.h"
#include "queue_fns.h"
#include "uxsocket.h"

static int queues_init(struct homa_slow *ctx, struct homa_app *app);
static int socket_close_poll(struct homa_slow *ctx, struct homa_app *app,
    __u32 sock_id);
static void disconnect_poll(struct homa_slow *ctx, struct homa_app *app);
static void app_disconnect(struct homa_slow *ctx, struct homa_app *app);
static void app_accept(struct homa_slow *ctx);
static int socket_open(struct homa_slow *ctx, struct homa_app *app,
    __u32 *sock_id);
static int socket_bind(struct homa_slow *ctx, struct homa_socket *sock,
    const struct homa_queue_entry *request);
static int control_apply(struct homa_slow *ctx, struct homa_app *app,
    const struct homa_queue_entry *request, __u32 *sock_id);
static void control_poll(struct homa_slow *ctx, struct homa_app *app);

int homa_app_init(struct homa_slow *ctx)
{
  struct sockaddr_un addr = { .sun_family = AF_UNIX };

  if (mkdir("/run/chamelio", 0755) != 0 && errno != EEXIST)
    return -1;

  ctx->listen_fd = socket(AF_UNIX,
      SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (ctx->listen_fd < 0)
    return -1;

  memcpy(addr.sun_path, HOMA_APP_SOCKET_PATH, sizeof(HOMA_APP_SOCKET_PATH));
  unlink(addr.sun_path);

  if (bind(ctx->listen_fd, (struct sockaddr *) &addr, sizeof(addr)) != 0 ||
      listen(ctx->listen_fd, HOMA_MAX_CONTEXTS) != 0)
  {
    close(ctx->listen_fd);
    ctx->listen_fd = -1;
    unlink(HOMA_APP_SOCKET_PATH);
    return -1;
  }

  return 0;
}

int homa_app_poll(struct homa_slow *ctx)
{
  struct pollfd fds[HOMA_MAX_CONTEXTS + 1] = { 0 };
  struct homa_app *app;
  unsigned int i;
  int ret;

  fds[0].fd = ctx->listen_fd;
  fds[0].events = POLLIN;
  for (i = 0; i < HOMA_MAX_CONTEXTS; i++)
  {
    fds[i + 1].fd = ctx->apps[i].app_fd;
    fds[i + 1].events = POLLIN;
  }

  ret = poll(fds, HOMA_MAX_CONTEXTS + 1, 0);
  if (ret < 0)
    return errno == EINTR ? 0 : -1;

  for (i = 0; i < HOMA_MAX_CONTEXTS; i++)
  {
    app = &ctx->apps[i];
    if (fds[i + 1].revents != 0)
      app_disconnect(ctx, app);

    if (app->disconnecting)
      disconnect_poll(ctx, app);
    else if (app->app_fd >= 0)
      control_poll(ctx, app);
  }

  if (fds[0].revents & POLLIN)
    app_accept(ctx);

  return 0;
}

static void app_disconnect(struct homa_slow *ctx, struct homa_app *app)
{
  if (app->app_fd < 0)
    return;

  cham_disable_queue(ctx->proto, app->app_fast_qid, 0);
  close(app->app_fd);
  app->app_fd = -1;
  app->disconnecting = 1;
}

static void app_accept(struct homa_slow *ctx)
{
  struct homa_app *app = NULL;
  unsigned int i;
  int fd;

  fd = accept4(ctx->listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
  if (fd < 0)
    return;

  for (i = 0; i < HOMA_MAX_CONTEXTS; i++)
  {
    if (i >= HOMA_CLIENT_RPCS / ctx->config.context_rpcs)
      break;

    if (ctx->apps[i].app_fd < 0 && !ctx->apps[i].disconnecting)
    {
      app = &ctx->apps[i];
      break;
    }
  }

  if (app == NULL)
  {
    close(fd);
    return;
  }

  if (queues_init(ctx, app) != 0)
  {
    close(fd);
    return;
  }

  ctx->contexts[i].app_qid = app->fast_app_qid;
  ctx->contexts[i].state = HOMA_CONTEXT_ACTIVE;
  app->app_fd = fd;
  if (uxsocket_sendfd(fd, ctx->proto->shm_fd, -1) < 0 ||
      send(fd, &app->info, sizeof(app->info), MSG_NOSIGNAL) !=
      sizeof(app->info))
    app_disconnect(ctx, app);
}

static int socket_open(struct homa_slow *ctx, struct homa_app *app,
    __u32 *sock_id)
{
  __u32 i;

  for (i = 0; i < HOMA_MAX_SOCKETS; i++)
  {
    if (ctx->sockets[i].state == HOMA_SOCKET_FREE)
    {
      ctx->sockets[i].state = HOMA_SOCKET_OPEN;
      ctx->sockets[i].app_qid = app->fast_app_qid;
      *sock_id = i;
      return 0;
    }
  }

  return EMFILE;
}

static int socket_bind(struct homa_slow *ctx, struct homa_socket *sock,
    const struct homa_queue_entry *request)
{
  __u32 local_ip = request->data.request.local_ip;
  __u32 port = request->data.request.local_port;

  if (sock->state != HOMA_SOCKET_OPEN)
    return EINVAL;

  if (local_ip != 0 && local_ip != ctx->proto->local_ip)
    return EADDRNOTAVAIL;

  if (port == 0)
  {
    for (port = 49152; port <= UINT16_MAX; port++)
    {
      if (ctx->ports[port] == 0)
        break;
    }

    if (port > UINT16_MAX)
      return EADDRINUSE;
  }

  if (ctx->ports[port] != 0)
    return EADDRINUSE;

  ctx->ports[port] = request->data.request.sock_id + 1;
  sock->local_ip = ctx->proto->local_ip;
  sock->local_port = port;
  sock->state = HOMA_SOCKET_BOUND;
  return 0;
}

static int control_apply(struct homa_slow *ctx, struct homa_app *app,
    const struct homa_queue_entry *request, __u32 *sock_id)
{
  struct homa_socket *sock;

  *sock_id = request->data.request.sock_id;

  if (request->type == HOMA_QUEUE_SOCKET)
    return socket_open(ctx, app, sock_id);

  if (*sock_id >= HOMA_MAX_SOCKETS)
    return EBADF;

  sock = &ctx->sockets[*sock_id];
  if (sock->state == HOMA_SOCKET_FREE ||
      sock->app_qid != app->fast_app_qid)
    return EBADF;

  switch (request->type)
  {
    case HOMA_QUEUE_BIND:
      return socket_bind(ctx, sock, request);
    case HOMA_QUEUE_CLOSE:
      return socket_close_poll(ctx, app, *sock_id);
    default:
      return EOPNOTSUPP;
  }
}

static void control_poll(struct homa_slow *ctx, struct homa_app *app)
{
  struct homa_queue_entry *request;
  struct homa_queue_entry *response;
  __u32 sock_id;
  int i;

  for (i = 0; i < 16; i++)
  {
    request = queue_head(&app->app_slow);
    response = queue_tail(&app->slow_app);
    if (request == NULL || response == NULL)
      break;

    response->data.result.status = control_apply(ctx, app, request, &sock_id);
    if (response->data.result.status == EINPROGRESS)
      break;
    response->data.result.sock_id = sock_id;
    queue_enqueue(&app->slow_app, HOMA_QUEUE_RESULT);
    queue_dequeue(&app->app_slow);
  }
}

static int socket_close_poll(struct homa_slow *ctx, struct homa_app *app,
    __u32 sock_id)
{
  struct homa_socket *sock = &ctx->sockets[sock_id];
  struct homa_queue_entry *entry;
  struct homa_rpc *rpc;
  unsigned int examined = 0;

  if (app->closing_sock < 0)
  {
    entry = queue_tail(&ctx->slow_fast);
    if (entry == NULL)
      return EINPROGRESS;

    app->closing_sock = sock_id;
    app->close_cursor = 0;
    app->close_barrier = 0;
    if (sock->state == HOMA_SOCKET_BOUND)
      ctx->ports[sock->local_port] = 0;

    entry->data.request.sock_id = sock_id;
    queue_enqueue(&ctx->slow_fast, HOMA_QUEUE_CLOSE_START);
  }

  MEM_BARRIER();
  if (sock->state != HOMA_SOCKET_CLOSING && sock->state != HOMA_SOCKET_CLOSED)
    return EINPROGRESS;

  while (app->close_cursor < HOMA_MAX_RPCS && examined < HOMA_REAP_BATCH)
  {
    rpc = &ctx->rpcs[app->close_cursor];
    if (rpc->state != HOMA_RPC_FREE && rpc->sock_id == sock_id &&
        rpc->app_qid == sock->app_qid)
    {
      entry = queue_tail(&ctx->slow_fast);
      if (entry == NULL)
        return EINPROGRESS;

      entry->data.rpc_handle = rpc->handle;
      queue_enqueue(&ctx->slow_fast, HOMA_QUEUE_REAP);
    }

    app->close_cursor++;
    examined++;
  }

  if (app->close_cursor < HOMA_MAX_RPCS)
    return EINPROGRESS;

  if (!app->close_barrier)
  {
    entry = queue_tail(&ctx->slow_fast);
    if (entry == NULL)
      return EINPROGRESS;

    entry->data.request.sock_id = sock_id;
    queue_enqueue(&ctx->slow_fast, HOMA_QUEUE_CLOSE_DONE);
    app->close_barrier = 1;
  }

  MEM_BARRIER();
  if (sock->state != HOMA_SOCKET_CLOSED)
    return EINPROGRESS;

  memset(sock, 0, sizeof(*sock));
  app->closing_sock = -1;
  return 0;
}

static void disconnect_poll(struct homa_slow *ctx, struct homa_app *app)
{
  unsigned int i;

  if (app->closing_sock >= 0 &&
      socket_close_poll(ctx, app, app->closing_sock) != 0)
    return;

  for (i = 0; i < HOMA_MAX_SOCKETS; i++)
  {
    if (ctx->sockets[i].state != HOMA_SOCKET_FREE &&
        ctx->sockets[i].app_qid == app->fast_app_qid)
    {
      socket_close_poll(ctx, app, i);
      return;
    }
  }

  if (!app->reset_pending)
  {
    struct homa_queue_entry *entry = queue_tail(&ctx->slow_fast);
    unsigned int id = app - ctx->apps;

    if (entry == NULL)
      return;

    ctx->contexts[id].state = HOMA_CONTEXT_CLOSING;
    entry->data.context_id = id;
    queue_enqueue(&ctx->slow_fast, HOMA_QUEUE_CONTEXT_RESET);
    app->reset_pending = 1;
  }

  if (__atomic_load_n(&ctx->contexts[app - ctx->apps].state,
      __ATOMIC_ACQUIRE) != HOMA_CONTEXT_FREE)
    return;

  app->reset_pending = 0;
  app->disconnecting = 0;
}

static int queues_init(struct homa_slow *ctx, struct homa_app *app)
{
  struct proto_queue_lib *queue;
  __u64 *offsets[] = {
    &app->info.app_slow_off,
    &app->info.slow_app_off,
    &app->info.app_fast_off,
    &app->info.fast_app_off,
  };
  unsigned int i;

  if (app->info.shm_len == 0)
  {
    if (ctx->proto->nqueues > MAX_PROTO_QUEUES - 4)
    {
      errno = ENOBUFS;
      return -1;
    }

    for (i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++)
    {
      queue = cham_new_queue(ctx->proto, HOMA_QUEUE_LENGTH,
          sizeof(struct homa_queue_entry));
      if (queue == NULL)
        return -1;

      *offsets[i] = queue->off;
      memset((char *) ctx->proto->shm_base + queue->off, 0,
          queue->nelems * queue->elsize);

      if (i == HOMA_APP_FAST_Q)
        app->app_fast_qid = queue->id;
      else if (i == HOMA_FAST_APP_Q)
        app->fast_app_qid = queue->id;
    }
  }
  else
  {
    for (i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++)
      memset((char *) ctx->proto->shm_base + *offsets[i], 0,
          HOMA_QUEUE_LENGTH * sizeof(struct homa_queue_entry));
  }

  app->info.rpc_begin = (app - ctx->apps) * ctx->config.context_rpcs;
  app->info.rpc_end = app->info.rpc_begin + ctx->config.context_rpcs;
  app->info.app_qid = app->fast_app_qid;
  app->info.sockets_off =
      (char *) ctx->sockets - (char *) ctx->proto->shm_base;
  app->info.rpcs_off = (char *) ctx->rpcs - (char *) ctx->proto->shm_base;
  app->info.shm_len = ctx->proto->shm_size;
  app->info.shm_off = ctx->proto->shm_off;

  if (cham_enable_queue(ctx->proto, app->app_fast_qid, 0) != 0)
    return -1;

  dqueue_init(&app->app_slow, HOMA_QUEUE_LENGTH,
      sizeof(struct homa_queue_entry),
      (char *) ctx->proto->shm_base + app->info.app_slow_off,
      app->info.app_slow_off);
  equeue_init(&app->slow_app, HOMA_QUEUE_LENGTH,
      sizeof(struct homa_queue_entry),
      (char *) ctx->proto->shm_base + app->info.slow_app_off,
      app->info.slow_app_off);

  return 0;
}

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

static int queues_init(struct homa_slow *ctx);
static int socket_close_poll(struct homa_slow *ctx, __u32 sock_id);
static void disconnect_poll(struct homa_slow *ctx);
static void app_disconnect(struct homa_slow *ctx);
static void app_accept(struct homa_slow *ctx);
static int socket_open(struct homa_slow *ctx, __u32 *sock_id);
static int socket_bind(struct homa_slow *ctx, struct homa_socket *sock,
    const struct homa_queue_entry *request);
static int control_apply(struct homa_slow *ctx,
    const struct homa_queue_entry *request, __u32 *sock_id);
static void control_poll(struct homa_slow *ctx);

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
      listen(ctx->listen_fd, 1) != 0)
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
  struct pollfd fds[] = {
    { .fd = ctx->listen_fd, .events = POLLIN },
    { .fd = ctx->app_fd, .events = POLLIN },
  };
  int ret;

  ret = poll(fds, 2, 0);
  if (ret < 0 && errno != EINTR)
    return -1;

  if (fds[1].revents != 0)
    app_disconnect(ctx);

  if (ctx->disconnecting)
    disconnect_poll(ctx);

  if (!ctx->disconnecting && (fds[0].revents & POLLIN))
    app_accept(ctx);

  if (ctx->app_fd >= 0)
    control_poll(ctx);

  return 0;
}

static void app_disconnect(struct homa_slow *ctx)
{
  if (ctx->app_fd < 0)
    return;

  cham_disable_queue(ctx->proto, ctx->app_fast_qid, 0);
  close(ctx->app_fd);
  ctx->app_fd = -1;
  ctx->disconnecting = 1;
  memset(ctx->ports, 0, (UINT16_MAX + 1U) * sizeof(*ctx->ports));
}

static void app_accept(struct homa_slow *ctx)
{
  int fd;

  fd = accept4(ctx->listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
  if (fd < 0)
    return;

  if (ctx->app_fd >= 0)
  {
    close(fd);
    return;
  }

  if (queues_init(ctx) != 0 ||
      uxsocket_sendfd(fd, ctx->proto->shm_fd, -1) < 0 ||
      send(fd, &ctx->info, sizeof(ctx->info), MSG_NOSIGNAL) !=
      sizeof(ctx->info))
  {
    close(fd);
    return;
  }

  ctx->app_fd = fd;
}

static int socket_open(struct homa_slow *ctx, __u32 *sock_id)
{
  __u32 i;

  for (i = 0; i < HOMA_MAX_SOCKETS; i++)
  {
    if (ctx->sockets[i].state == HOMA_SOCKET_FREE)
    {
      ctx->sockets[i].state = HOMA_SOCKET_OPEN;
      ctx->sockets[i].app_qid = ctx->fast_app_qid;
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

static int control_apply(struct homa_slow *ctx,
    const struct homa_queue_entry *request, __u32 *sock_id)
{
  struct homa_socket *sock;

  *sock_id = request->data.request.sock_id;

  if (request->type == HOMA_QUEUE_SOCKET)
    return socket_open(ctx, sock_id);

  if (*sock_id >= HOMA_MAX_SOCKETS)
    return EBADF;

  sock = &ctx->sockets[*sock_id];
  if (sock->state == HOMA_SOCKET_FREE)
    return EBADF;

  switch (request->type)
  {
    case HOMA_QUEUE_BIND:
      return socket_bind(ctx, sock, request);
    case HOMA_QUEUE_CLOSE:
      return socket_close_poll(ctx, *sock_id);
    default:
      return EOPNOTSUPP;
  }
}

static void control_poll(struct homa_slow *ctx)
{
  struct homa_queue_entry *request;
  struct homa_queue_entry *response;
  __u32 sock_id;
  int i;

  for (i = 0; i < 16; i++)
  {
    request = queue_head(&ctx->app_slow);
    response = queue_tail(&ctx->slow_app);
    if (request == NULL || response == NULL)
      break;

    response->data.result.status = control_apply(ctx, request, &sock_id);
    if (response->data.result.status == EINPROGRESS)
      break;
    response->data.result.sock_id = sock_id;
    queue_enqueue(&ctx->slow_app, HOMA_QUEUE_RESULT);
    queue_dequeue(&ctx->app_slow);
  }
}

static int socket_close_poll(struct homa_slow *ctx, __u32 sock_id)
{
  struct homa_socket *sock = &ctx->sockets[sock_id];
  struct homa_queue_entry *entry;
  struct homa_rpc *rpc;
  unsigned int examined = 0;

  if (ctx->closing_sock < 0)
  {
    entry = queue_tail(&ctx->slow_fast);
    if (entry == NULL)
      return EINPROGRESS;

    ctx->closing_sock = sock_id;
    ctx->close_cursor = 0;
    ctx->close_barrier = 0;
    if (sock->state == HOMA_SOCKET_BOUND)
      ctx->ports[sock->local_port] = 0;

    entry->data.request.sock_id = sock_id;
    queue_enqueue(&ctx->slow_fast, HOMA_QUEUE_CLOSE_START);
  }

  MEM_BARRIER();
  if (sock->state != HOMA_SOCKET_CLOSING && sock->state != HOMA_SOCKET_CLOSED)
    return EINPROGRESS;

  while (ctx->close_cursor < HOMA_MAX_RPCS && examined < HOMA_REAP_BATCH)
  {
    rpc = &ctx->rpcs[ctx->close_cursor];
    if (rpc->state != HOMA_RPC_FREE && rpc->sock_id == sock_id &&
        rpc->app_qid == sock->app_qid)
    {
      entry = queue_tail(&ctx->slow_fast);
      if (entry == NULL)
        return EINPROGRESS;

      entry->data.rpc_handle = rpc->handle;
      queue_enqueue(&ctx->slow_fast, HOMA_QUEUE_REAP);
    }

    ctx->close_cursor++;
    examined++;
  }

  if (ctx->close_cursor < HOMA_MAX_RPCS)
    return EINPROGRESS;

  if (!ctx->close_barrier)
  {
    entry = queue_tail(&ctx->slow_fast);
    if (entry == NULL)
      return EINPROGRESS;

    entry->data.request.sock_id = sock_id;
    queue_enqueue(&ctx->slow_fast, HOMA_QUEUE_CLOSE_DONE);
    ctx->close_barrier = 1;
  }

  MEM_BARRIER();
  if (sock->state != HOMA_SOCKET_CLOSED)
    return EINPROGRESS;

  memset(sock, 0, sizeof(*sock));
  ctx->closing_sock = -1;
  return 0;
}

static void disconnect_poll(struct homa_slow *ctx)
{
  unsigned int i;

  if (ctx->closing_sock >= 0 &&
      socket_close_poll(ctx, ctx->closing_sock) != 0)
    return;

  for (i = 0; i < HOMA_MAX_SOCKETS; i++)
  {
    if (ctx->sockets[i].state != HOMA_SOCKET_FREE)
    {
      socket_close_poll(ctx, i);
      return;
    }
  }

  ctx->disconnecting = 0;
}

static int queues_init(struct homa_slow *ctx)
{
  struct proto_queue_lib *queue;
  __u64 *offsets[] = {
    &ctx->info.app_slow_off,
    &ctx->info.slow_app_off,
    &ctx->info.app_fast_off,
    &ctx->info.fast_app_off,
  };
  unsigned int i;

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
    {
      ctx->app_fast_qid = queue->id;
      if (cham_enable_queue(ctx->proto, queue->id, 0) != 0)
        return -1;
    }
    else if (i == HOMA_FAST_APP_Q)
    {
      ctx->fast_app_qid = queue->id;
    }
  }

  dqueue_init(&ctx->app_slow, HOMA_QUEUE_LENGTH,
      sizeof(struct homa_queue_entry),
      (char *) ctx->proto->shm_base + ctx->info.app_slow_off,
      ctx->info.app_slow_off);
  equeue_init(&ctx->slow_app, HOMA_QUEUE_LENGTH,
      sizeof(struct homa_queue_entry),
      (char *) ctx->proto->shm_base + ctx->info.slow_app_off,
      ctx->info.slow_app_off);

  return 0;
}

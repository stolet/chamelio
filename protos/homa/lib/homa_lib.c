#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/un.h>
#include <unistd.h>

#include "homa_lib.h"
#include "homa.h"
#include "homa_hdr.h"
#include "homa_queue_types.h"
#include "queue_fns.h"
#include "uxsocket.h"

struct homa_context_lib {
  void *shm_base;
  size_t shm_len;
  struct homa_socket *sockets;
  struct homa_rpc *rpcs;
  __u64 delivered[HOMA_MAX_RPCS];
  __u8 received[HOMA_MAX_RPCS];
  __u8 replied[HOMA_MAX_RPCS];
  struct equeue app_slow;
  struct dqueue slow_app;
  struct equeue app_fast;
  struct dqueue fast_app;
};

static int slow_fd = -1;
static struct homa_context_lib *context;

static int slow_alive(void);
static struct homa_socket *socket_get(struct homa_context_lib *ctx, int sockfd);
static struct homa_rpc *rpc_get(struct homa_context_lib *ctx, int sockfd,
    __u64 rpc_id);
static int message_valid(const void *data, size_t len);
static __u32 rpc_slot(__u64 handle);
static int control_request(struct homa_context_lib *ctx, __u8 type,
    __u32 sock_id, __u32 local_ip, __u16 local_port);
static int queue_offset_valid(const struct homa_context_info *info, __u64 off);

int homa_lib_connect_slow(void)
{
  struct sockaddr_un addr = { .sun_family = AF_UNIX };
  int fd;

  if (slow_fd >= 0)
  {
    errno = EALREADY;
    return -1;
  }

  fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -1;

  memcpy(addr.sun_path, HOMA_APP_SOCKET_PATH, sizeof(HOMA_APP_SOCKET_PATH));

  if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) != 0)
  {
    int saved_errno = errno;

    close(fd);
    errno = saved_errno;
    return -1;
  }

  slow_fd = fd;
  return 0;
}

struct homa_context_lib *homa_lib_ctx_new(void)
{
  struct homa_context_info info;
  struct homa_context_lib *ctx;
  int shm_fd = -1;
  int saved_errno;
  int64_t marker;
  size_t received = 0;
  ssize_t ret;

  if (slow_fd < 0 || context != NULL)
  {
    errno = slow_fd < 0 ? ENOTCONN : EALREADY;
    return NULL;
  }

  if (uxsocket_read_one_msg(slow_fd, &marker, &shm_fd) != 0 ||
      marker != -1 || shm_fd < 0)
  {
    errno = EPROTO;
    goto failed;
  }

  while (received < sizeof(info))
  {
    ret = recv(slow_fd, (char *) &info + received, sizeof(info) - received, 0);
    if (ret < 0 && errno == EINTR)
      continue;

    if (ret <= 0)
    {
      if (ret == 0)
        errno = ECONNRESET;

      goto failed;
    }

    received += ret;
  }

  if (info.shm_len == 0 || !queue_offset_valid(&info, info.app_slow_off) ||
      !queue_offset_valid(&info, info.slow_app_off) ||
      !queue_offset_valid(&info, info.app_fast_off) ||
      !queue_offset_valid(&info, info.fast_app_off) ||
      info.sockets_off > info.shm_len ||
      sizeof(struct homa_socket) * HOMA_MAX_SOCKETS >
          info.shm_len - info.sockets_off ||
      info.rpcs_off > info.shm_len ||
      sizeof(struct homa_rpc) * HOMA_MAX_RPCS > info.shm_len - info.rpcs_off)
  {
    errno = EPROTO;
    goto failed;
  }

  ctx = calloc(1, sizeof(*ctx));
  if (ctx == NULL)
    goto failed;

  ctx->shm_base = mmap(NULL, info.shm_len, PROT_READ | PROT_WRITE,
      MAP_SHARED, shm_fd, info.shm_off);
  if (ctx->shm_base == MAP_FAILED)
  {
    free(ctx);
    goto failed;
  }

  close(shm_fd);
  ctx->shm_len = info.shm_len;
  ctx->sockets = (void *) ((char *) ctx->shm_base + info.sockets_off);
  ctx->rpcs = (void *) ((char *) ctx->shm_base + info.rpcs_off);

  equeue_init(&ctx->app_slow, HOMA_QUEUE_LENGTH,
      sizeof(struct homa_queue_entry),
      (char *) ctx->shm_base + info.app_slow_off, info.app_slow_off);
  dqueue_init(&ctx->slow_app, HOMA_QUEUE_LENGTH,
      sizeof(struct homa_queue_entry),
      (char *) ctx->shm_base + info.slow_app_off, info.slow_app_off);
  equeue_init(&ctx->app_fast, HOMA_QUEUE_LENGTH,
      sizeof(struct homa_queue_entry),
      (char *) ctx->shm_base + info.app_fast_off, info.app_fast_off);
  dqueue_init(&ctx->fast_app, HOMA_QUEUE_LENGTH,
      sizeof(struct homa_queue_entry),
      (char *) ctx->shm_base + info.fast_app_off, info.fast_app_off);

  context = ctx;
  return ctx;

failed:
  saved_errno = errno;

  if (shm_fd >= 0)
    close(shm_fd);

  homa_lib_disconnect_slow();
  errno = saved_errno;
  return NULL;
}

void homa_lib_disconnect_slow(void)
{
  if (context != NULL)
  {
    munmap(context->shm_base, context->shm_len);
    free(context);
    context = NULL;
  }

  if (slow_fd >= 0)
  {
    close(slow_fd);
    slow_fd = -1;
  }
}

void homa_lib_ctx_free(struct homa_context_lib *ctx)
{
  if (ctx != NULL && ctx == context)
    homa_lib_disconnect_slow();
}

int homa_lib_socket(struct homa_context_lib *ctx)
{
  return control_request(ctx, HOMA_QUEUE_SOCKET, 0, 0, 0);
}

int homa_lib_bind(struct homa_context_lib *ctx, int sockfd,
    const struct sockaddr *addr, socklen_t addrlen)
{
  const struct sockaddr_in *in = (const struct sockaddr_in *) addr;

  if (addr == NULL || addrlen < sizeof(*in) || sockfd < 0)
  {
    errno = EINVAL;
    return -1;
  }

  if (in->sin_family != AF_INET)
  {
    errno = EAFNOSUPPORT;
    return -1;
  }

  if (control_request(ctx, HOMA_QUEUE_BIND, sockfd,
      ntohl(in->sin_addr.s_addr), ntohs(in->sin_port)) < 0)
    return -1;

  return 0;
}

int homa_lib_close(struct homa_context_lib *ctx, int sockfd)
{
  if (sockfd < 0)
  {
    errno = EBADF;
    return -1;
  }

  if (control_request(ctx, HOMA_QUEUE_CLOSE, sockfd, 0, 0) < 0)
    return -1;

  return 0;
}

int homa_lib_request(struct homa_context_lib *ctx, int sockfd, const void *data,
    size_t len, const struct sockaddr *peer, socklen_t peerlen, __u64 *rpc_id)
{
  const struct sockaddr_in *in = (const struct sockaddr_in *) peer;
  struct homa_queue_entry *entry;
  struct homa_rpc *rpc;
  __u32 i;

  if (socket_get(ctx, sockfd) == NULL || !message_valid(data, len))
    return -1;

  if (peer == NULL || peerlen < sizeof(*in) || rpc_id == NULL)
  {
    errno = EINVAL;
    return -1;
  }

  if (in->sin_family != AF_INET)
  {
    errno = EAFNOSUPPORT;
    return -1;
  }

  if (in->sin_port == 0 || in->sin_addr.s_addr == INADDR_ANY)
  {
    errno = EINVAL;
    return -1;
  }

  entry = queue_tail(&ctx->app_fast);
  if (entry == NULL)
  {
    errno = EAGAIN;
    return -1;
  }

  for (i = 0; i < HOMA_CLIENT_RPCS; i++)
  {
    if (ctx->rpcs[i].state == HOMA_RPC_FREE)
      break;
  }

  if (i == HOMA_CLIENT_RPCS)
  {
    errno = ENOBUFS;
    return -1;
  }

  rpc = &ctx->rpcs[i];
  if (rpc->handle == 0)
    rpc->handle = i + 1;
  else
    rpc->handle += HOMA_MAX_RPCS;

  rpc->id = rpc->handle * HOMA_RPC_ID_STRIDE;
  rpc->flags = 0;
  rpc->rx_len = 0;
  rpc->rx_bitmap = 0;
  ctx->delivered[i] = 0;
  ctx->received[i] = 0;
  ctx->replied[i] = 0;
  rpc->sock_id = sockfd;
  rpc->app_qid = ctx->sockets[sockfd].app_qid;
  rpc->remote_ip = ntohl(in->sin_addr.s_addr);
  rpc->remote_port = ntohs(in->sin_port);
  rpc->tx_len = len;
  if (len != 0)
    memcpy(rpc->tx_data, data, len);

  rpc->state = HOMA_RPC_WAIT_RESPONSE;
  entry->data.rpc_handle = rpc->handle;
  queue_enqueue(&ctx->app_fast, HOMA_QUEUE_TX);
  *rpc_id = rpc->handle;
  return 0;
}

int homa_lib_reply(struct homa_context_lib *ctx, int sockfd, __u64 rpc_id,
    const void *data, size_t len)
{
  struct homa_rpc *rpc;
  struct homa_queue_entry *entry;
  __u32 slot;

  rpc = rpc_get(ctx, sockfd, rpc_id);
  if (rpc == NULL || !message_valid(data, len))
    return -1;

  slot = rpc_slot(rpc_id);
  if (!(rpc->id & HOMA_RPC_SERVER_BIT) || ctx->replied[slot])
  {
    errno = EINVAL;
    return -1;
  }

  entry = queue_tail(&ctx->app_fast);
  if (entry == NULL)
  {
    errno = EAGAIN;
    return -1;
  }

  if (len != 0)
    memcpy(rpc->tx_data, data, len);

  rpc->tx_len = len;
  entry->data.rpc_handle = rpc_id;
  queue_enqueue(&ctx->app_fast, HOMA_QUEUE_TX);
  ctx->replied[slot] = 1;
  return 0;
}

int homa_lib_poll(struct homa_context_lib *ctx, struct homa_event *events,
    int maxevents)
{
  struct homa_queue_entry *entry;
  struct homa_event *event;
  struct homa_rpc *rpc;
  __u32 slot;
  __u64 handle;
  int n = 0;

  if (ctx == NULL || ctx != context || slow_fd < 0)
  {
    errno = ENOTCONN;
    return -1;
  }

  if (events == NULL || maxevents <= 0)
  {
    errno = EINVAL;
    return -1;
  }

  while (n < maxevents && (entry = queue_head(&ctx->fast_app)) != NULL)
  {
    handle = entry->data.rpc_handle;
    if (entry->type != HOMA_QUEUE_RX || handle == 0)
    {
      errno = EPROTO;
      return -1;
    }

    slot = rpc_slot(handle);
    MEM_BARRIER();
    rpc = &ctx->rpcs[slot];
    if (rpc->handle != handle || rpc->state == HOMA_RPC_FREE ||
        rpc->app_qid != ctx->sockets[rpc->sock_id].app_qid)
    {
      queue_dequeue(&ctx->fast_app);
      continue;
    }

    event = &events[n++];
    memset(event, 0, sizeof(*event));
    event->type = (rpc->id & HOMA_RPC_SERVER_BIT) ?
        HOMA_EVENT_REQUEST : HOMA_EVENT_RESPONSE;
    event->sockfd = rpc->sock_id;
    event->rpc_id = handle;
    event->peer.sin_family = AF_INET;
    event->peer.sin_addr.s_addr = htonl(rpc->remote_ip);
    event->peer.sin_port = htons(rpc->remote_port);
    event->data = rpc->rx_data;
    event->len = rpc->rx_len;
    ctx->delivered[slot] = handle;
    ctx->received[slot] = 1;
    ctx->replied[slot] = 0;
    queue_dequeue(&ctx->fast_app);
  }

  return n;
}

int homa_lib_release(struct homa_context_lib *ctx, int sockfd, __u64 rpc_id)
{
  struct homa_queue_entry *entry;
  __u32 slot;

  if (rpc_get(ctx, sockfd, rpc_id) == NULL)
    return -1;

  slot = rpc_slot(rpc_id);
  if (!ctx->received[slot])
  {
    errno = EINVAL;
    return -1;
  }

  entry = queue_tail(&ctx->app_fast);
  if (entry == NULL)
  {
    errno = EAGAIN;
    return -1;
  }

  entry->data.rpc_handle = rpc_id;
  queue_enqueue(&ctx->app_fast, HOMA_QUEUE_RELEASE);
  ctx->received[slot] = 0;
  return 0;
}

static struct homa_socket *socket_get(struct homa_context_lib *ctx, int sockfd)
{
  if (ctx == NULL || ctx != context || slow_fd < 0)
  {
    errno = ENOTCONN;
    return NULL;
  }

  if (sockfd < 0 || sockfd >= HOMA_MAX_SOCKETS ||
      ctx->sockets[sockfd].state == HOMA_SOCKET_FREE)
  {
    errno = EBADF;
    return NULL;
  }

  if (ctx->sockets[sockfd].state != HOMA_SOCKET_BOUND)
  {
    errno = EDESTADDRREQ;
    return NULL;
  }

  return &ctx->sockets[sockfd];
}

static struct homa_rpc *rpc_get(struct homa_context_lib *ctx, int sockfd,
    __u64 rpc_id)
{
  struct homa_rpc *rpc;
  __u32 slot;

  if (socket_get(ctx, sockfd) == NULL)
    return NULL;

  if (rpc_id == 0)
  {
    errno = EINVAL;
    return NULL;
  }

  slot = rpc_slot(rpc_id);
  rpc = &ctx->rpcs[slot];
  if (rpc->handle != rpc_id || rpc->state == HOMA_RPC_FREE ||
      rpc->sock_id != (__u32) sockfd || ctx->delivered[slot] != rpc_id)
  {
    errno = EINVAL;
    return NULL;
  }

  return rpc;
}

static __u32 rpc_slot(__u64 handle)
{
  return (handle - 1) % HOMA_MAX_RPCS;
}

static int message_valid(const void *data, size_t len)
{
  if (len > HOMA_MESSAGE_BYTES)
  {
    errno = EMSGSIZE;
    return 0;
  }

  if (len != 0 && data == NULL)
  {
    errno = EINVAL;
    return 0;
  }

  return 1;
}

static int slow_alive(void)
{
  char byte;
  ssize_t ret;

  ret = recv(slow_fd, &byte, sizeof(byte), MSG_PEEK | MSG_DONTWAIT);
  if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
    return 0;

  errno = ret == 0 ? ECONNRESET : EPROTO;
  return -1;
}

static int control_request(struct homa_context_lib *ctx, __u8 type,
    __u32 sock_id, __u32 local_ip, __u16 local_port)
{
  struct homa_queue_entry *entry;
  int status;
  int result;

  if (ctx == NULL || ctx != context || slow_fd < 0)
  {
    errno = ENOTCONN;
    return -1;
  }

  entry = queue_tail(&ctx->app_slow);
  if (entry == NULL)
  {
    errno = EAGAIN;
    return -1;
  }

  entry->data.request.sock_id = sock_id;
  entry->data.request.local_ip = local_ip;
  entry->data.request.local_port = local_port;

  if (queue_enqueue(&ctx->app_slow, type) != 0)
  {
    errno = EAGAIN;
    return -1;
  }

  while ((entry = queue_head(&ctx->slow_app)) == NULL)
  {
    if (slow_alive() != 0)
      return -1;
  }

  if (entry->type != HOMA_QUEUE_RESULT)
  {
    queue_dequeue(&ctx->slow_app);
    errno = EPROTO;
    return -1;
  }

  status = entry->data.result.status;
  result = entry->data.result.sock_id;
  queue_dequeue(&ctx->slow_app);

  if (status != 0)
  {
    errno = status;
    return -1;
  }

  return result;
}

static int queue_offset_valid(const struct homa_context_info *info, __u64 off)
{
  size_t len = HOMA_QUEUE_LENGTH * sizeof(struct homa_queue_entry);

  return off <= info->shm_len && len <= info->shm_len - off;
}

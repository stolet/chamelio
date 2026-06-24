#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdatomic.h>

#include <cham_lib.h>

#include "tcp_lib.h"
#include "queue_fns.h"
#include "tcp_queue_types.h"
#include "log.h"
#include "uxsocket.h"
#include "internal.h"
#include "utils_sync.h"

#define LIB_BATCH_SIZE 16
#define TCP_TX_BUMP_THRESH 1024U

static struct tcp_lib *tcp = NULL;

static int handle_new_sock_res(struct tcp_queue_entry *qe);
static int handle_bind_res(struct tcp_queue_entry *qe);
static int handle_setopt_res(struct tcp_queue_entry *qe);
static int handle_connect_res(struct tcp_queue_entry *qe);
static int handle_listen_res(struct tcp_queue_entry *qe);
static int handle_listen_newconn(struct tcp_queue_entry *qe);
static int handle_accept_res(struct tcp_queue_entry *qe);
static int handle_tx_bump(struct tcp_queue_bump_entry *qe);
static int handle_rx_bump(struct tcp_queue_bump_entry *qe);

static struct tcp_socket_lib *alloc_lib_sock(struct tcp_context_lib *ctx);
static struct tcp_socket_lib *lookup_sock(struct tcp_context_lib *ctx,
    int sockfd);
static void sock_mark_bump(struct tcp_context_lib *ctx,
    struct tcp_socket_lib *sock);
static void sock_flush_bumps(struct tcp_context_lib *ctx,
    struct tcp_socket_lib *sock);
static void tcp_flush_bumps(struct tcp_context_lib *ctx);
static __u32 sock_wait_events(struct tcp_socket_lib *sock);
static int wait_poll_ctx(struct tcp_context_lib *ctx, int mode);
static int wait_collect(struct tcp_wait *wait,
    struct tcp_wait_event *events,
    int maxevents);
static int wait(struct tcp_wait *wait,
    struct tcp_wait_event *events,
    int maxevents, int flags, int mode);
static void wait_until(struct tcp_context_lib *ctx,
    struct tcp_socket_lib *sock, __u32 events, int mode);
static int validate_sockaddr_in(const struct sockaddr *addr,
    socklen_t addrlen);
static int validate_nonblock_flags(int flags);
static void fill_sockaddr(struct tcp_socket_lib *sock, struct sockaddr *addr,
    socklen_t *addrlen);

static __u32 sock_rx_bump_thresh(const struct tcp_socket_lib *sock)
{
  __u32 thresh;

  thresh = sock->rx_len / 4;
  if (thresh == 0)
    thresh = 1;

  return thresh;
}

enum tcp_wait_poll_mode {
  TCP_WAIT_POLL_BOTH = 0,
  TCP_WAIT_POLL_FAST,
  TCP_WAIT_POLL_SLOW,
};

int tcp_connect_slow()
{
  int i;
  struct tcp_lib *t;
  struct sockaddr_un s_un;
  int shm_fd, ret, sock_fd;
  int64_t tmp;

  if (tcp != NULL)
  {
    LOG_WARN("library already connected to tcp slow-path");
    return -1;
  }

  sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock_fd < 0)
  {
    LOG_ERROR("failed to create socket");
    return -1;
  }

  s_un.sun_family = AF_UNIX;
  ret = snprintf(s_un.sun_path, sizeof(s_un.sun_path), "%s", TCP_APP_SOCKET_PATH);
  if (ret < 0 || ret >= (int) sizeof(s_un.sun_path))
  {
    LOG_ERROR("could not copy unix socket path");
    goto close_sockfd;
  }

  if (connect(sock_fd, (struct sockaddr *) &s_un, sizeof(s_un)) < 0)
  {
    LOG_ERROR("cannot connect to slow-path, %s", TCP_APP_SOCKET_PATH);
    perror("");
    goto close_sockfd;
  }

  if (uxsocket_read_one_msg(sock_fd, &tmp, &shm_fd) < 0 ||
      tmp != -1 || shm_fd < 0)
  {
    if (shm_fd >= 0)
      close(shm_fd);

    LOG_ERROR("cannot read shared memory fd from slow-path");
    goto close_sockfd;
  }

  t = malloc(sizeof(struct tcp_lib));
  if (t == NULL)
  {
    LOG_ERROR("failed to allocate tcp_lib struct");
    goto close_sockfd;
  }

  memset(t, 0, sizeof(*t));
  for (i = 0; i < MAX_SOCKETS; i++)
    t->socks[i].fd = SOCK_INACTIVE;

  t->uxsocket_fd = sock_fd;
  t->shm_fd = shm_fd;
  t->shm_base = NULL;
  t->next_ctxid = 0;
  t->next_sockfd = 0;
  t->lock = 0;
  tcp = t;

  return 0;

close_sockfd:
  close(sock_fd);
  return -1;
}

struct tcp_context_lib * tcp_ctx_new()
{
  int i;
  ssize_t sz, off;
  void *shm_base;
  struct tcp_context_lib *ctx;
  struct tcp_queue_new_actx_res *res;
  __u8 resp_buf[sizeof(*res)];
  struct equeue *eq, **eq_list;
  struct dqueue *dq, **dq_list;
  struct tcp_queue_new_actx_req req = {
    .req = 1,
  };
  struct iovec iov = {
    .iov_base = &req,
    .iov_len = sizeof(req),
  };
  struct msghdr msg = {
    .msg_name = NULL,
    .msg_namelen = 0,
    .msg_iov = &iov,
    .msg_iovlen = 1,
    .msg_control = NULL,
    .msg_controllen = 0,
    .msg_flags = 0,
  };

  util_spin_lock(&tcp->lock);
  sz = sendmsg(tcp->uxsocket_fd, &msg, 0);
  if (sz != sizeof(req))
  {
    LOG_ERROR("failed to send msg to register tcp app ctx");
    perror("");
    util_spin_unlock(&tcp->lock);
    return NULL;
  }

  res = (struct tcp_queue_new_actx_res *) resp_buf;
  off = 0;
  while (off < (ssize_t) sizeof(*res))
  {
    sz = read(tcp->uxsocket_fd, (__u8 *) res + off, sizeof(*res) - off);
    if (sz < 0)
    {
      LOG_ERROR("read failed");
      perror("");
      util_spin_unlock(&tcp->lock);
      return NULL;
    }
    off += sz;
  }

  ctx = malloc(sizeof(struct tcp_context_lib));
  if (ctx == NULL)
  {
    LOG_ERROR("failed to allocate tcp context struct");
    util_spin_unlock(&tcp->lock);
    return NULL;
  }

  ctx->id = __sync_fetch_and_add(&tcp->next_ctxid, 1);
  ctx->ncores = res->n_fp_cores;
  ctx->bump_head = TCP_BUMP_NONE;
  ctx->bump_tail = TCP_BUMP_NONE;

  if (ctx->id == 0)
  {
    shm_base = mmap(NULL, res->shm_len, PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_POPULATE, tcp->shm_fd, res->shm_off);
    if (shm_base == (void *) -1)
    {
      LOG_ERROR("failed to map shm region");
      util_spin_unlock(&tcp->lock);
      return NULL;
    }
    tcp->shm_base = shm_base;
  }

  while (__atomic_load_n(&tcp->shm_base, __ATOMIC_SEQ_CST) == NULL) {}

  eq = equeue_new(res->as_nelems, res->as_elsize,
      tcp->shm_base + res->as_off, res->as_off);
  if (eq == NULL)
  {
    LOG_ERROR("failed to create queue from app to slow-path");
    util_spin_unlock(&tcp->lock);
    return NULL;
  }
  ctx->app_slow_q = eq;

  dq = dqueue_new(res->sa_nelems, res->sa_elsize,
      tcp->shm_base + res->sa_off, res->sa_off);
  if (dq == NULL)
  {
    LOG_ERROR("failed to create queue from slow-path to app");
    util_spin_unlock(&tcp->lock);
    return NULL;
  }
  ctx->slow_app_q = dq;

  eq_list = malloc(sizeof(struct equeue *) * res->n_fp_cores);
  if (eq_list == NULL)
  {
    LOG_ERROR("failed to allocate list for queues app->fast");
    util_spin_unlock(&tcp->lock);
    return NULL;
  }
  ctx->app_fast_qs = eq_list;

  dq_list = malloc(sizeof(struct dqueue *) * res->n_fp_cores);
  if (dq_list == NULL)
  {
    LOG_ERROR("failed to allocate list for queues fast->app");
    util_spin_unlock(&tcp->lock);
    return NULL;
  }
  ctx->fast_app_qs = dq_list;

  for (i = 0; i < (int) res->n_fp_cores; i++)
  {
    eq = equeue_new(res->af_nelems, res->af_elsize,
        tcp->shm_base + res->af_offs[i], res->af_offs[i]);
    if (eq == NULL)
    {
      LOG_ERROR("failed to create app->fast bump queue");
      util_spin_unlock(&tcp->lock);
      return NULL;
    }
    eq_list[i] = eq;

    dq = dqueue_new(res->fa_nelems, res->fa_elsize,
        tcp->shm_base + res->fa_offs[i], res->fa_offs[i]);
    if (dq == NULL)
    {
      LOG_ERROR("failed to create fast->app bump queue");
      util_spin_unlock(&tcp->lock);
      return NULL;
    }
    dq_list[i] = dq;
  }

  util_spin_unlock(&tcp->lock);
  return ctx;
}

struct tcp_wait *tcp_wait_new(struct tcp_context_lib *ctx)
{
  int i;
  struct tcp_wait *wait;

  if (tcp == NULL)
  {
    errno = ENOTCONN;
    return NULL;
  }

  if (ctx == NULL)
  {
    errno = EINVAL;
    return NULL;
  }

  wait = malloc(sizeof(*wait));
  if (wait == NULL)
  {
    errno = ENOMEM;
    return NULL;
  }

  wait->ctx = ctx;
  wait->nfds = 0;
  for (i = 0; i < MAX_SOCKETS; i++)
  {
    wait->entries[i].events = 0;
    wait->entries[i].data = 0;
    wait->entries[i].index = -1;
    wait->entries[i].sock = NULL;
  }

  return wait;
}

void tcp_wait_free(struct tcp_wait *wait)
{
  free(wait);
}

int tcp_wait_add(struct tcp_wait *wait, int sockfd, __u32 events, __u64 data)
{
  struct tcp_socket_lib *sock;

  if (wait == NULL)
  {
    errno = EINVAL;
    return -1;
  }

  sock = lookup_sock(wait->ctx, sockfd);
  if (sock == NULL)
    return -1;

  if (wait->entries[sockfd].index != -1)
  {
    errno = EEXIST;
    return -1;
  }

  wait->entries[sockfd].events = events;
  wait->entries[sockfd].data = data;
  wait->entries[sockfd].index = wait->nfds;
  wait->entries[sockfd].sock = sock;
  wait->sockfds[wait->nfds] = sockfd;
  wait->nfds++;
  return 0;
}

int tcp_wait_mod(struct tcp_wait *wait, int sockfd, __u32 events, __u64 data)
{
  struct tcp_socket_lib *sock;

  if (wait == NULL)
  {
    errno = EINVAL;
    return -1;
  }

  sock = lookup_sock(wait->ctx, sockfd);
  if (sock == NULL)
    return -1;

  if (wait->entries[sockfd].index == -1)
  {
    errno = ENOENT;
    return -1;
  }

  wait->entries[sockfd].events = events;
  wait->entries[sockfd].data = data;
  wait->entries[sockfd].sock = sock;
  return 0;
}

int tcp_wait_del(struct tcp_wait *wait, int sockfd)
{
  int idx, last_fd;

  if (wait == NULL)
  {
    errno = EINVAL;
    return -1;
  }

  if (sockfd < 0 || sockfd >= MAX_SOCKETS)
  {
    errno = EBADF;
    return -1;
  }

  idx = wait->entries[sockfd].index;
  if (idx == -1)
  {
    errno = ENOENT;
    return -1;
  }

  wait->nfds--;
  last_fd = wait->sockfds[wait->nfds];
  wait->sockfds[idx] = last_fd;
  wait->entries[last_fd].index = idx;
  wait->entries[sockfd].index = -1;
  wait->entries[sockfd].events = 0;
  wait->entries[sockfd].data = 0;
  wait->entries[sockfd].sock = NULL;
  return 0;
}

int tcp_wait(struct tcp_wait *waito, struct tcp_wait_event *events,
    int maxevents, int flags)
{
  return wait(waito, events, maxevents, flags, TCP_WAIT_POLL_BOTH);
}

int tcp_wait_fast(struct tcp_wait *waito,
    struct tcp_wait_event *events,
    int maxevents, int flags)
{
  return wait(waito, events, maxevents, flags, TCP_WAIT_POLL_FAST);
}

int tcp_wait_slow(struct tcp_wait *waito,
    struct tcp_wait_event *events,
    int maxevents, int flags)
{
  return wait(waito, events, maxevents, flags, TCP_WAIT_POLL_SLOW);
}

int tcp_socket(struct tcp_context_lib *ctx)
{
  int ret;
  struct tcp_socket_lib *sock;
  struct equeue *q;
  struct tcp_queue_entry *qe;
  struct tcp_queue_new_sock_req *req;

  if (ctx == NULL)
  {
    LOG_ERROR("passed NULL tcp_context_lib");
    errno = EINVAL;
    return -1;
  }

  sock = alloc_lib_sock(ctx);
  if (sock == NULL)
    return -1;

  q = ctx->app_slow_q;
  qe = queue_tail(q);
  if (qe == NULL)
  {
    LOG_ERROR("failed to get queue tail");
    sock->fd = SOCK_INACTIVE;
    errno = EAGAIN;
    return -1;
  }

  req = &qe->data.new_sock_req;
  req->opaque = (__u64) sock;
  ret = queue_enqueue(q, TCP_QUEUE_NEW_SOCK_REQ);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue new sock req");
    sock->fd = SOCK_INACTIVE;
    errno = EAGAIN;
    return -1;
  }

  wait_until(ctx, sock, TCP_WAIT_SOCKET, TCP_WAIT_POLL_SLOW);

  return sock->fd;
}

int tcp_bind(struct tcp_context_lib *ctx, int sockfd,
    const struct sockaddr *addr, socklen_t addrlen)
{
  int ret;
  struct equeue *q;
  struct tcp_queue_entry *qe;
  struct tcp_queue_bind_req *req;
  struct sockaddr_in *sin;
  struct tcp_socket_lib *sock;

  if (validate_sockaddr_in(addr, addrlen) != 0)
    return -1;

  sock = lookup_sock(ctx, sockfd);
  if (sock == NULL)
    return -1;

  sin = (struct sockaddr_in *) addr;
  q = ctx->app_slow_q;
  qe = queue_tail(q);
  if (qe == NULL)
  {
    LOG_ERROR("failed to get queue tail");
    errno = EAGAIN;
    return -1;
  }

  req = &qe->data.bind_req;
  req->sock_id = sock->sock_id;
  req->local_ip = ntohl(sin->sin_addr.s_addr);
  req->local_port = ntohs(sin->sin_port);
  req->opaque = (__u64) sock;
  ret = queue_enqueue(q, TCP_QUEUE_BIND_REQ);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue bind req");
    errno = EAGAIN;
    return -1;
  }

  wait_until(ctx, sock, TCP_WAIT_BIND, TCP_WAIT_POLL_SLOW);

  if (!sock->bind_success)
  {
    errno = EADDRINUSE;
    sock->bind_success = -1;
    return -1;
  }

  sock->local_port = ntohs(sin->sin_port);
  sock->local_ip = ntohl(sin->sin_addr.s_addr);
  sock->state = TCP_LIB_STATE_BOUND;
  sock->bind_success = -1;
  return 0;
}

int tcp_connect(struct tcp_context_lib *ctx, int sockfd,
    const struct sockaddr *addr, socklen_t addrlen, int flags)
{
  int ret;
  struct equeue *q;
  struct tcp_queue_entry *qe;
  struct tcp_queue_connect_req *req;
  struct sockaddr_in *sin;
  struct tcp_socket_lib *sock;

  if (validate_nonblock_flags(flags) != 0)
    return -1;
  if (validate_sockaddr_in(addr, addrlen) != 0)
    return -1;

  sock = lookup_sock(ctx, sockfd);
  if (sock == NULL)
    return -1;

  if (sock->state == TCP_LIB_STATE_LISTEN)
  {
    errno = EOPNOTSUPP;
    return -1;
  }

  if (sock->state == TCP_LIB_STATE_ESTABLISHED)
    return 0;

  if (sock->state == TCP_LIB_STATE_CONNECTING)
  {
    if (sock->op_status == TCP_LIB_STATUS_PENDING)
    {
      if ((flags & SOCK_NONBLOCK) != 0)
      {
        errno = EALREADY;
        return -1;
      }

      wait_until(ctx, sock, TCP_WAIT_OP, TCP_WAIT_POLL_SLOW);
    }

    if (sock->op_status == 0 && sock->state == TCP_LIB_STATE_ESTABLISHED)
      return 0;

    if (sock->op_status > 0)
    {
      errno = sock->op_status;
      sock->op_status = TCP_LIB_STATUS_IDLE;
      return -1;
    }
  }

  sin = (struct sockaddr_in *) addr;
  q = ctx->app_slow_q;
  qe = queue_tail(q);
  if (qe == NULL)
  {
    errno = EAGAIN;
    return -1;
  }

  req = &qe->data.connect_req;
  req->sock_id = sock->sock_id;
  req->remote_ip = ntohl(sin->sin_addr.s_addr);
  req->remote_port = ntohs(sin->sin_port);
  req->opaque = (__u64) sock;

  LOG_DEBUG("tcp_connect: sock_id=%u remote_ip=%08x remote_port=%u sin_addr=%08x",
      req->sock_id, req->remote_ip, req->remote_port, sin->sin_addr.s_addr);

  ret = queue_enqueue(q, TCP_QUEUE_CONNECT_REQ);
  if (ret != 0)
  {
    errno = EAGAIN;
    return -1;
  }

  sock->remote_ip = ntohl(sin->sin_addr.s_addr);
  sock->remote_port = ntohs(sin->sin_port);
  sock->state = TCP_LIB_STATE_CONNECTING;
  sock->op_status = TCP_LIB_STATUS_PENDING;

  if ((flags & SOCK_NONBLOCK) != 0)
  {
    errno = EINPROGRESS;
    return -1;
  }

  wait_until(ctx, sock, TCP_WAIT_OP, TCP_WAIT_POLL_SLOW);

  if (sock->op_status != 0)
  {
    errno = sock->op_status;
    sock->op_status = TCP_LIB_STATUS_IDLE;
    return -1;
  }

  return 0;
}

int tcp_listen(struct tcp_context_lib *ctx, int sockfd, int backlog)
{
  int ret;
  struct equeue *q;
  struct tcp_queue_entry *qe;
  struct tcp_queue_listen_req *req;
  struct tcp_socket_lib *sock;

  sock = lookup_sock(ctx, sockfd);
  if (sock == NULL)
    return -1;

  if (backlog <= 0)
  {
    errno = EINVAL;
    return -1;
  }

  if (sock->local_port == 0 || sock->state == TCP_LIB_STATE_CONNECTING ||
      sock->state == TCP_LIB_STATE_ESTABLISHED)
  {
    errno = EINVAL;
    return -1;
  }

  if (sock->state == TCP_LIB_STATE_LISTEN)
    return 0;

  if (sock->op_status == TCP_LIB_STATUS_PENDING)
  {
    errno = EALREADY;
    return -1;
  }

  if (sock->op_status > 0)
  {
    errno = sock->op_status;
    sock->op_status = TCP_LIB_STATUS_IDLE;
    return -1;
  }

  q = ctx->app_slow_q;
  qe = queue_tail(q);
  if (qe == NULL)
  {
    errno = EAGAIN;
    return -1;
  }

  req = &qe->data.listen_req;
  req->sock_id = sock->sock_id;
  req->backlog = backlog;
  req->opaque = (__u64) sock;

  ret = queue_enqueue(q, TCP_QUEUE_LISTEN_REQ);
  if (ret != 0)
  {
    errno = EAGAIN;
    return -1;
  }

  sock->op_status = TCP_LIB_STATUS_PENDING;
  return 0;
}

int tcp_accept(struct tcp_context_lib *ctx, int sockfd,
    struct sockaddr *addr, socklen_t *addrlen, int flags)
{
  int ret;
  struct equeue *q;
  struct tcp_queue_entry *qe;
  struct tcp_queue_accept_req *req;
  struct tcp_socket_lib *listen_sock;
  struct tcp_socket_lib *sock;

  if (validate_nonblock_flags(flags) != 0)
    return -1;

  listen_sock = lookup_sock(ctx, sockfd);
  if (listen_sock == NULL)
    return -1;

  if (listen_sock->state != TCP_LIB_STATE_LISTEN)
  {
    errno = EINVAL;
    return -1;
  }

  if ((flags & SOCK_NONBLOCK) != 0 && listen_sock->pending_conn == 0)
  {
    errno = EAGAIN;
    return -1;
  }

  wait_until(ctx, listen_sock, TCP_WAIT_ACCEPT, TCP_WAIT_POLL_SLOW);

  sock = alloc_lib_sock(ctx);
  if (sock == NULL)
    return -1;

  q = ctx->app_slow_q;
  qe = queue_tail(q);
  if (qe == NULL)
  {
    sock->fd = SOCK_INACTIVE;
    errno = EAGAIN;
    return -1;
  }

  req = &qe->data.accept_req;
  req->sock_id = listen_sock->sock_id;
  req->opaque = (__u64) sock;
  ret = queue_enqueue(q, TCP_QUEUE_ACCEPT_REQ);
  if (ret != 0)
  {
    sock->fd = SOCK_INACTIVE;
    errno = EAGAIN;
    return -1;
  }

  sock->op_status = TCP_LIB_STATUS_PENDING;
  wait_until(ctx, sock, TCP_WAIT_OP, TCP_WAIT_POLL_SLOW);

  if (sock->op_status != 0)
  {
    errno = sock->op_status;
    sock->fd = SOCK_INACTIVE;
    sock->state = TCP_LIB_STATE_CLOSED;
    return -1;
  }

  fill_sockaddr(sock, addr, addrlen);
  return sock->fd;
}

int tcp_setsockopt(struct tcp_context_lib *ctx, int sockfd, __u8 opt)
{
  int ret;
  struct equeue *q;
  struct tcp_queue_entry *qe;
  struct tcp_queue_setopt_req *req;
  struct tcp_socket_lib *sock;

  sock = lookup_sock(ctx, sockfd);
  if (sock == NULL)
    return -1;

  q = ctx->app_slow_q;
  qe = queue_tail(q);
  if (qe == NULL)
  {
    LOG_ERROR("failed to get queue tail");
    errno = EAGAIN;
    return -1;
  }

  req = &qe->data.setopt_req;
  req->opt = opt;
  req->sock_id = sock->sock_id;
  req->opaque = (__u64) sock;

  ret = queue_enqueue(q, TCP_QUEUE_SETOPT_REQ);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue setsockopt req");
    errno = EAGAIN;
    return -1;
  }

  wait_until(ctx, sock, TCP_WAIT_SETOPT, TCP_WAIT_POLL_SLOW);

  if (!sock->setopt_success)
  {
    errno = ENOPROTOOPT;
    sock->setopt_success = -1;
    return -1;
  }

  if (opt == SO_REUSEPORT)
    sock->reuseport = 1;
  sock->setopt_success = -1;
  return 0;
}

int tcp_sendto(struct tcp_context_lib *ctx, int sockfd,
    const void *buf, size_t len,
    const struct sockaddr *addr, socklen_t addrlen)
{
  __u32 tx_inflight;
  __u32 tail, n, n1, n2;
  __u32 tx_len, tx_avail, tx_head;
  __u32 tx_ip;
  __u16 tx_port;
  __u8 *tx_buf;
  const __u8 *src;
  struct tcp_socket_lib *sock;
  struct sockaddr_in sa;
  struct sockaddr_in *sin;

  sock = lookup_sock(ctx, sockfd);
  if (sock == NULL)
    return -1;

  if (sock->state != TCP_LIB_STATE_ESTABLISHED)
  {
    errno = ENOTCONN;
    return -1;
  }

  if (addr == NULL)
  {
    if (sock->remote_port == 0)
    {
      errno = ENOTCONN;
      return -1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(sock->remote_ip);
    sa.sin_port = htons(sock->remote_port);
    sin = &sa;
  }
  else
  {
    if (validate_sockaddr_in(addr, addrlen) != 0)
      return -1;
    sin = (struct sockaddr_in *) addr;
  }

  if (len == 0)
    return 0;

  tx_len = sock->tx_len;
  tx_avail = sock->tx_avail;
  tx_head = sock->tx_head;
  tx_buf = sock->tx_buf;
  tail = tx_head + tx_avail;
  src = buf;

  utils_prefetch0(tx_buf + tail);

  n = len;
  if (n > tx_len - tx_avail)
    n = tx_len - tx_avail;

  if (n == 0)
  {
    errno = EAGAIN;
    return -1;
  }

  if (tail >= tx_len)
    tail -= tx_len;
  tx_ip = ntohl(sin->sin_addr.s_addr);
  tx_port = ntohs(sin->sin_port);

  if (tail + n > tx_len)
  {
    n1 = tx_len - tail;
    n2 = n - n1;
    memcpy(tx_buf + tail, src, n1);
    memcpy(tx_buf, src + n1, n2);
  }
  else
  {
    memcpy(tx_buf + tail, src, n);
  }

  sock->tx_avail = tx_avail + n;
  sock->remote_ip = tx_ip;
  sock->remote_port = tx_port;
  sock->tx_bump_pending += n;
  sock_mark_bump(ctx, sock);

  tx_inflight = tx_avail - sock->tx_bump_pending + n;
  if (tx_inflight == 0 || sock->tx_bump_pending >= TCP_TX_BUMP_THRESH)
    sock_flush_bumps(ctx, sock);

  return n;
}

int tcp_recvfrom(struct tcp_context_lib *ctx, int sockfd,
    void *buf, size_t len,
    struct sockaddr *addr, socklen_t addr_len)
{
  int n;
  __u32 n1, n2, new_head;
  __u32 rx_len, rx_avail, rx_head;
  __u32 rx_was_full;
  __u8 *rx_buf;
  struct tcp_socket_lib *sock;
  struct sockaddr_in *sin = (struct sockaddr_in *) addr;

  sock = lookup_sock(ctx, sockfd);
  if (sock == NULL)
    return -1;

  if (sock->state != TCP_LIB_STATE_ESTABLISHED &&
      sock->state != TCP_LIB_STATE_CLOSING)
  {
    errno = ENOTCONN;
    return -1;
  }

  if (addr != NULL && addr_len < sizeof(struct sockaddr_in))
  {
    errno = EINVAL;
    return -1;
  }

  rx_len = sock->rx_len;
  rx_avail = sock->rx_avail;
  rx_head = sock->rx_head;
  rx_buf = sock->rx_buf;

  n = len;
  if ((__u32) n > rx_avail)
    n = rx_avail;

  if (n == 0)
  {
    errno = EAGAIN;
    return -1;
  }

  if (addr != NULL)
  {
    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = htonl(sock->remote_ip);
    sin->sin_port = htons(sock->remote_port);
  }
  rx_was_full = (rx_avail == rx_len);

  new_head = rx_head + n;
  if (new_head >= rx_len)
    new_head -= rx_len;

  if (rx_head + n > rx_len)
  {
    n1 = rx_len - rx_head;
    n2 = n - n1;
    memcpy(buf, rx_buf + rx_head, n1);
    memcpy((__u8 *) buf + n1, rx_buf, n2);
  }
  else
  {
    memcpy(buf, rx_buf + rx_head, n);
  }

  sock->rx_avail = rx_avail - n;
  sock->rx_head = new_head;
  sock->rx_bump_pending += n;
  sock_mark_bump(ctx, sock);

  if (rx_was_full || sock->rx_bump_pending >= sock_rx_bump_thresh(sock))
    sock_flush_bumps(ctx, sock);

  return n;
}

int tcp_shutdown(struct tcp_context_lib *ctx, int sockfd, int how)
{
  int ret;
  struct equeue *q;
  struct tcp_queue_entry *qe;
  struct tcp_queue_shutdown_req *req;
  struct tcp_socket_lib *sock;

  sock = lookup_sock(ctx, sockfd);
  if (sock == NULL)
    return -1;

  if (how != SHUT_RDWR)
  {
    errno = EINVAL;
    return -1;
  }

  if (sock->state != TCP_LIB_STATE_ESTABLISHED &&
      sock->state != TCP_LIB_STATE_CLOSING)
  {
    errno = ENOTCONN;
    return -1;
  }

  tcp_flush_bumps(ctx);

  q = ctx->app_slow_q;
  qe = queue_tail(q);
  if (qe == NULL)
  {
    errno = EAGAIN;
    return -1;
  }

  req = &qe->data.shutdown_req;
  req->sock_id = sock->sock_id;
  req->how = how;
  req->opaque = (__u64) sock;

  ret = queue_enqueue(q, TCP_QUEUE_SHUTDOWN_REQ);
  if (ret != 0)
  {
    errno = EAGAIN;
    return -1;
  }

  sock->state = TCP_LIB_STATE_CLOSING;
  return 0;
}

int tcp_close(struct tcp_context_lib *ctx, int sockfd)
{
  int ret;
  struct equeue *q;
  struct tcp_queue_entry *qe;
  struct tcp_queue_close_req *req;
  struct tcp_socket_lib *sock;

  sock = lookup_sock(ctx, sockfd);
  if (sock == NULL)
    return -1;

  tcp_flush_bumps(ctx);

  q = ctx->app_slow_q;
  qe = queue_tail(q);
  if (qe != NULL)
  {
    req = &qe->data.close_req;
    req->sock_id = sock->sock_id;
    req->opaque = (__u64) sock;
    ret = queue_enqueue(q, TCP_QUEUE_CLOSE_REQ);
    if (ret != 0)
    {
      errno = EAGAIN;
      return -1;
    }
  }

  sock->state = TCP_LIB_STATE_CLOSED;
  sock->fd = SOCK_INACTIVE;
  return 0;
}

int tcp_poll_fast(struct tcp_context_lib *ctx)
{
  int i, n, ncores;
  struct dqueue *q;
  struct tcp_queue_bump_entry *qe;
  struct dqueue **fast_app_qs;

  tcp_flush_bumps(ctx);

  n = 0;
  ncores = ctx->ncores;
  fast_app_qs = ctx->fast_app_qs;
  for (i = 0; i < ncores; i++)
  {
    q = fast_app_qs[i];
    while ((qe = queue_head(q)) != NULL)
    {
      __u32 next_head = q->head + q->elsize;
      if (next_head >= (q->elsize * q->nelems))
        next_head = 0;
      utils_prefetch0((__u8 *) q->entries + next_head);

      n++;
      switch (qe->type)
      {
        case TCP_QUEUE_BUMP_APP_TX:
          handle_tx_bump(qe);
          break;
        case TCP_QUEUE_BUMP_APP_RX:
          handle_rx_bump(qe);
          break;
        default:
          LOG_ERROR("unknown queue entry type from fast-path to app type=%d",
              qe->type);
          abort();
      }

      queue_dequeue(q);
    }
  }

  return n;
}

int tcp_poll_slow(struct tcp_context_lib *ctx)
{
  int n;
  struct dqueue *q;
  struct tcp_queue_entry *qe;

  n = 0;
  q = ctx->slow_app_q;
  while (n < LIB_BATCH_SIZE)
  {
    qe = queue_head(q);
    if (qe == NULL)
      return -1;

    n++;
    switch (qe->type)
    {
      case TCP_QUEUE_NEW_SOCK_RES:
        handle_new_sock_res(qe);
        break;
      case TCP_QUEUE_BIND_RES:
        handle_bind_res(qe);
        break;
      case TCP_QUEUE_SETOPT_RES:
        handle_setopt_res(qe);
        break;
      case TCP_QUEUE_CONNECT_RES:
        handle_connect_res(qe);
        break;
      case TCP_QUEUE_LISTEN_RES:
        handle_listen_res(qe);
        break;
      case TCP_QUEUE_LISTEN_NEWCONN:
        handle_listen_newconn(qe);
        break;
      case TCP_QUEUE_ACCEPT_RES:
        handle_accept_res(qe);
        break;
      default:
        LOG_ERROR("unknown queue entry type from slow-path to app type=%d",
            qe->type);
        abort();
    }
    queue_dequeue(q);
  }

  return n;
}

static int handle_new_sock_res(struct tcp_queue_entry *qe)
{
  struct tcp_queue_new_sock_res *res;
  struct tcp_socket_lib *sock;

  res = &qe->data.new_sock_res;
  sock = (struct tcp_socket_lib *) res->opaque;
  sock->core = res->core;
  sock->sock_id = res->sock_id;
  sock->rx_qid = res->rx_qid;
  sock->rx_len = res->rx_len;
  sock->rx_buf = tcp->shm_base + res->rx_off;
  sock->tx_qid = res->tx_qid;
  sock->tx_len = res->tx_len;
  sock->tx_buf = tcp->shm_base + res->tx_off;

  return 0;
}

static int handle_bind_res(struct tcp_queue_entry *qe)
{
  struct tcp_queue_bind_res *res;
  struct tcp_socket_lib *sock;

  res = &qe->data.bind_res;
  sock = (struct tcp_socket_lib *) res->opaque;
  sock->bind_success = res->success;
  return 0;
}

static int handle_setopt_res(struct tcp_queue_entry *qe)
{
  struct tcp_queue_setopt_res *res;
  struct tcp_socket_lib *sock;

  res = &qe->data.setopt_res;
  sock = (struct tcp_socket_lib *) res->opaque;
  sock->setopt_success = res->success;
  return 0;
}

static int handle_connect_res(struct tcp_queue_entry *qe)
{
  struct tcp_queue_connect_res *res;
  struct tcp_socket_lib *sock;

  res = &qe->data.connect_res;
  sock = (struct tcp_socket_lib *) res->opaque;
  if (sock->state == TCP_LIB_STATE_CLOSED || sock->fd == SOCK_INACTIVE)
    return 0;

  sock->op_status = res->status;
  if (res->status == 0)
  {
    sock->core = res->core;
    sock->local_ip = res->local_ip;
    sock->local_port = res->local_port;
    sock->remote_ip = res->remote_ip;
    sock->remote_port = res->remote_port;
    sock->state = TCP_LIB_STATE_ESTABLISHED;
  }
  else if (sock->local_port != 0)
  {
    sock->state = TCP_LIB_STATE_BOUND;
  }
  else
  {
    sock->state = TCP_LIB_STATE_INIT;
  }

  return 0;
}

static int handle_listen_res(struct tcp_queue_entry *qe)
{
  struct tcp_queue_listen_res *res;
  struct tcp_socket_lib *sock;

  res = &qe->data.listen_res;
  sock = (struct tcp_socket_lib *) res->opaque;
  if (sock->state == TCP_LIB_STATE_CLOSED || sock->fd == SOCK_INACTIVE)
    return 0;

  sock->op_status = res->status;
  if (res->status == 0)
    sock->state = TCP_LIB_STATE_LISTEN;
  else if (sock->local_port != 0)
    sock->state = TCP_LIB_STATE_BOUND;
  else
    sock->state = TCP_LIB_STATE_INIT;

  return 0;
}

static int handle_listen_newconn(struct tcp_queue_entry *qe)
{
  struct tcp_queue_listen_newconn *res;
  struct tcp_socket_lib *sock;

  res = &qe->data.listen_newconn;
  sock = (struct tcp_socket_lib *) res->opaque;
  if (sock->state != TCP_LIB_STATE_LISTEN)
    return 0;

  sock->pending_conn++;
  return 0;
}

static int handle_accept_res(struct tcp_queue_entry *qe)
{
  struct tcp_queue_accept_res *res;
  struct tcp_socket_lib *listen_sock;
  struct tcp_socket_lib *sock;

  res = &qe->data.accept_res;
  sock = (struct tcp_socket_lib *) res->opaque;
  if (sock->fd == SOCK_INACTIVE && sock->state == TCP_LIB_STATE_CLOSED)
    return 0;

  listen_sock = (struct tcp_socket_lib *) res->listen_opaque;
  if (listen_sock != NULL && listen_sock->pending_conn > 0)
    listen_sock->pending_conn--;

  sock->op_status = res->status;
  if (res->status != 0)
    return 0;

  sock->core = res->core;
  sock->sock_id = res->sock_id;
  sock->rx_qid = res->rx_qid;
  sock->rx_len = res->rx_len;
  sock->rx_buf = tcp->shm_base + res->rx_off;
  sock->tx_qid = res->tx_qid;
  sock->tx_len = res->tx_len;
  sock->tx_buf = tcp->shm_base + res->tx_off;
  sock->local_ip = res->local_ip;
  sock->local_port = res->local_port;
  sock->remote_ip = res->remote_ip;
  sock->remote_port = res->remote_port;
  sock->state = TCP_LIB_STATE_ESTABLISHED;

  return 0;
}

static int handle_tx_bump(struct tcp_queue_bump_entry *qe)
{
  __u32 tx_bump;
  __u32 new_head;
  struct tcp_queue_bump_app_tx *bump;
  struct tcp_socket_lib *sock;

  bump = &qe->data.bump_app_tx;
  sock = (struct tcp_socket_lib *) bump->opaque;

  if (sock == NULL || sock->fd == SOCK_INACTIVE || sock->tx_len == 0)
    return 0;

  tx_bump = bump->tx_head;
  if (tx_bump > sock->tx_avail)
    tx_bump = sock->tx_avail;

  if (tx_bump == 0)
    return 0;

  new_head = sock->tx_head + tx_bump;
  if (new_head >= sock->tx_len)
    new_head -= sock->tx_len;
  sock->tx_head = new_head;
  sock->tx_avail -= tx_bump;

  if (sock->tx_bump_pending != 0 && sock->ctx != NULL &&
      sock->fd != SOCK_INACTIVE && sock->tx_avail == sock->tx_bump_pending)
  {
    sock_flush_bumps(sock->ctx, sock);
  }

  return 0;
}

static int handle_rx_bump(struct tcp_queue_bump_entry *qe)
{
  __u32 free_bytes, rx_bump;
  struct tcp_queue_bump_app_rx *bump;
  struct tcp_socket_lib *sock;

  bump = &qe->data.bump_app_rx;
  sock = (struct tcp_socket_lib *) bump->opaque;
  if (sock == NULL || sock->fd == SOCK_INACTIVE || sock->rx_len == 0)
    return 0;

  rx_bump = bump->rx_avail;
  free_bytes = sock->rx_len - sock->rx_avail;
  if (rx_bump > free_bytes)
    rx_bump = free_bytes;

  if (rx_bump == 0)
    return 0;

  utils_prefetch0(sock->rx_buf + sock->rx_head);
  sock->rx_avail += rx_bump;
  sock->remote_port = bump->rx_port;
  sock->remote_ip = bump->rx_ip;

  return 0;
}

static struct tcp_socket_lib *lookup_sock(struct tcp_context_lib *ctx,
    int sockfd)
{
  struct tcp_socket_lib *sock;

  if (tcp == NULL)
  {
    errno = ENOTCONN;
    return NULL;
  }

  if (ctx == NULL)
  {
    LOG_ERROR("passed NULL tcp_context_lib");
    errno = EINVAL;
    return NULL;
  }

  if (sockfd < 0 || sockfd >= MAX_SOCKETS)
  {
    errno = EBADF;
    return NULL;
  }

  sock = &tcp->socks[sockfd];
  if (sock->fd != sockfd || sock->ctx != ctx ||
      sock->state == TCP_LIB_STATE_CLOSED)
  {
    errno = EBADF;
    return NULL;
  }

  return sock;
}

static void sock_mark_bump(struct tcp_context_lib *ctx,
    struct tcp_socket_lib *sock)
{
  if (sock->bump_pending)
    return;

  sock->bump_pending = 1;
  sock->bump_next = TCP_BUMP_NONE;
  if (ctx->bump_tail == TCP_BUMP_NONE)
  {
    ctx->bump_head = sock->fd;
  }
  else
  {
    tcp->socks[ctx->bump_tail].bump_next = sock->fd;
  }
  ctx->bump_tail = sock->fd;
}

static void sock_flush_bumps(struct tcp_context_lib *ctx,
    struct tcp_socket_lib *sock)
{
  struct equeue *q;
  struct tcp_queue_bump_entry *qe;
  struct tcp_queue_bump_cham_tx *tx_bump;
  struct tcp_queue_bump_cham_rx *rx_bump;

  if (sock->ctx != ctx || sock->fd == SOCK_INACTIVE)
    return;

  q = ctx->app_fast_qs[sock->core];

  if (sock->tx_bump_pending != 0)
  {
    qe = queue_tail(q);
    if (qe == NULL)
      goto out;

    tx_bump = &qe->data.bump_cham_tx;
    tx_bump->sock_id = sock->sock_id;
    tx_bump->tx_ip = sock->remote_ip;
    tx_bump->tx_port = sock->remote_port;
    tx_bump->tx_avail = sock->tx_bump_pending;
    if (queue_enqueue(q, TCP_QUEUE_BUMP_CHAM_TX) == 0)
      sock->tx_bump_pending = 0;
  }

  if (sock->rx_bump_pending != 0)
  {
    qe = queue_tail(q);
    if (qe == NULL)
      goto out;

    rx_bump = &qe->data.bump_cham_rx;
    rx_bump->sock_id = sock->sock_id;
    rx_bump->rx_head = sock->rx_bump_pending;
    if (queue_enqueue(q, TCP_QUEUE_BUMP_CHAM_RX) == 0)
      sock->rx_bump_pending = 0;
  }

out:
  if (sock->tx_bump_pending != 0 || sock->rx_bump_pending != 0)
    sock_mark_bump(ctx, sock);
}

static void tcp_flush_bumps(struct tcp_context_lib *ctx)
{
  int fd, next;
  struct tcp_socket_lib *sock;

  fd = ctx->bump_head;
  ctx->bump_head = TCP_BUMP_NONE;
  ctx->bump_tail = TCP_BUMP_NONE;

  while (fd != TCP_BUMP_NONE)
  {
    sock = &tcp->socks[fd];
    next = sock->bump_next;
    sock->bump_pending = 0;
    sock->bump_next = TCP_BUMP_NONE;

    if (sock->ctx == ctx && sock->fd != SOCK_INACTIVE &&
        (sock->tx_bump_pending != 0 || sock->rx_bump_pending != 0))
    {
      sock_flush_bumps(ctx, sock);
    }

    fd = next;
  }
}

static __u32 sock_wait_events(struct tcp_socket_lib *sock)
{
  __u32 events = 0;

  if (sock == NULL || sock->fd == SOCK_INACTIVE)
    return 0;

  if (sock->rx_len != 0)
    events |= TCP_WAIT_SOCKET;
  if (sock->bind_success != -1)
    events |= TCP_WAIT_BIND;
  if (sock->setopt_success != -1)
    events |= TCP_WAIT_SETOPT;
  if (sock->op_status != TCP_LIB_STATUS_IDLE &&
      sock->op_status != TCP_LIB_STATUS_PENDING)
  {
    events |= TCP_WAIT_OP;
  }

  if (sock->state == TCP_LIB_STATE_LISTEN && sock->pending_conn > 0)
    events |= TCP_WAIT_IN | TCP_WAIT_ACCEPT;

  if (sock->state == TCP_LIB_STATE_ESTABLISHED ||
      sock->state == TCP_LIB_STATE_CLOSING)
  {
    if (sock->rx_avail > 0)
      events |= TCP_WAIT_IN;
    if (sock->state == TCP_LIB_STATE_ESTABLISHED &&
        sock->tx_len != 0 && sock->tx_avail < sock->tx_len)
    {
      events |= TCP_WAIT_OUT;
    }
  }

  if (sock->state == TCP_LIB_STATE_ESTABLISHED && sock->op_status == 0)
    events |= TCP_WAIT_CONNECTED;

  if (sock->state == TCP_LIB_STATE_CLOSING ||
      sock->state == TCP_LIB_STATE_CLOSED)
  {
    events |= TCP_WAIT_CLOSED;
  }

  return events;
}

static int wait_poll_ctx(struct tcp_context_lib *ctx, int mode)
{
  int n = 0;

  tcp_flush_bumps(ctx);

  if (mode != TCP_WAIT_POLL_FAST)
  {
    if (tcp_poll_slow(ctx) > 0)
      n++;
  }

  if (mode != TCP_WAIT_POLL_SLOW)
    n += tcp_poll_fast(ctx);

  return n;
}

static int wait_collect(struct tcp_wait *wait,
    struct tcp_wait_event *events,
    int maxevents)
{
  int i, n, sockfd;
  __u32 ready;
  struct tcp_socket_lib *sock;
  struct tcp_wait_entry *entry;

  n = 0;
  for (i = 0; i < wait->nfds && n < maxevents; i++)
  {
    sockfd = wait->sockfds[i];
    entry = &wait->entries[sockfd];
    sock = entry->sock;
    if (sock == NULL || sock->fd == SOCK_INACTIVE)
      continue;

    ready = sock_wait_events(sock) & entry->events;
    if (ready == 0)
      continue;

    events[n].sockfd = sockfd;
    events[n].events = ready;
    events[n].data = entry->data;
    n++;
  }

  return n;
}

static int wait(struct tcp_wait *wait,
    struct tcp_wait_event *events,
    int maxevents, int flags, int mode)
{
  int n;

  if (wait == NULL || events == NULL || maxevents <= 0)
  {
    errno = EINVAL;
    return -1;
  }

  for (;;)
  {
    n = wait_collect(wait, events, maxevents);
    if (n > 0)
      return n;

    wait_poll_ctx(wait->ctx, mode);

    n = wait_collect(wait, events, maxevents);
    if (n > 0)
      return n;

    if ((flags & TCP_WAIT_NONBLOCK) != 0)
      return 0;
  }
}

static void wait_until(struct tcp_context_lib *ctx,
    struct tcp_socket_lib *sock, __u32 events, int mode)
{
  while ((sock_wait_events(sock) & events) == 0)
  {
    wait_poll_ctx(ctx, mode);
  }
}

static int validate_sockaddr_in(const struct sockaddr *addr,
    socklen_t addrlen)
{
  if (addr == NULL || addrlen < sizeof(struct sockaddr_in))
  {
    errno = EINVAL;
    return -1;
  }

  if (addr->sa_family != AF_INET)
  {
    errno = EAFNOSUPPORT;
    return -1;
  }

  return 0;
}

static int validate_nonblock_flags(int flags)
{
  if ((flags & ~SOCK_NONBLOCK) != 0)
  {
    errno = EINVAL;
    return -1;
  }

  return 0;
}

static void fill_sockaddr(struct tcp_socket_lib *sock, struct sockaddr *addr,
    socklen_t *addrlen)
{
  struct sockaddr_in *sin;

  if (addr == NULL || addrlen == NULL)
    return;

  if (*addrlen < sizeof(struct sockaddr_in))
    return;

  sin = (struct sockaddr_in *) addr;
  memset(sin, 0, sizeof(*sin));
  sin->sin_family = AF_INET;
  sin->sin_addr.s_addr = htonl(sock->remote_ip);
  sin->sin_port = htons(sock->remote_port);
  *addrlen = sizeof(*sin);
}

static struct tcp_socket_lib *alloc_lib_sock(struct tcp_context_lib *ctx)
{
  int fd;
  struct tcp_socket_lib *sock;

  fd = __sync_fetch_and_add(&tcp->next_sockfd, 1);
  if (fd >= MAX_SOCKETS)
  {
    LOG_ERROR("exceeded max number of sockets");
    errno = EMFILE;
    return NULL;
  }

  sock = &tcp->socks[fd];
  memset(sock, 0, sizeof(*sock));
  sock->fd = fd;
  sock->ctx = ctx;
  sock->bump_next = TCP_BUMP_NONE;
  sock->bind_success = -1;
  sock->setopt_success = -1;
  sock->op_status = TCP_LIB_STATUS_IDLE;
  sock->state = TCP_LIB_STATE_INIT;

  return sock;
}

void tcp_set_debug(int enable)
{
  log_set_level(enable ? CHAM_LOG_LEVEL_DEBUG : CHAM_LOG_LEVEL_INFO);
}

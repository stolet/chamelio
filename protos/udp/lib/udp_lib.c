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

#include <cham_lib.h>

#include "udp_lib.h"
#include "queue_fns.h"
#include "udp_queue_types.h"
#include "log.h"
#include "uxsocket.h"

#define LIB_BATCH_SIZE 16

static struct udp_lib *udp = NULL;

static int handle_new_sock_res(struct udp_queue_entry *qe);
static int handle_tx_bump(struct udp_queue_bump_entry *qe);
static int handle_rx_bump(struct udp_queue_bump_entry *qe);
static int handle_bind_res(struct udp_queue_entry *qe);
static int handle_setopt_res(struct udp_queue_entry *qe);

int udp_connect_slow()
{
  struct udp_lib *u;
  struct sockaddr_un s_un;
  int shm_fd, ret, sock_fd;
  int64_t tmp;

  if (udp != NULL)
  {
    LOG_WARN("library already connected to udp slow-path");
    return -1;
  }
  
  sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock_fd < 0) 
  {
    LOG_ERROR("failed to create socket");
    return -1;
  }

  s_un.sun_family = AF_UNIX;
  ret = snprintf(s_un.sun_path, sizeof(s_un.sun_path), 
      "%s", APP_SOCKET_PATH);
  if (ret < 0 || ret >= sizeof(s_un.sun_path)) 
  {
    LOG_ERROR("could not copy unix socket path");
    goto close_sockfd;
  }

  if (connect(sock_fd, (struct sockaddr *)&s_un, sizeof(s_un)) < 0) 
  {
    LOG_ERROR("cannot connect to slow-path, %s", APP_SOCKET_PATH);
    perror("");
    goto close_sockfd;
  }
  
  /* Get shared mem fd */
  if (uxsocket_read_one_msg(sock_fd, &tmp, &shm_fd) < 0 || tmp != -1 || shm_fd < 0) 
  {
    if (shm_fd >= 0) 
      close(shm_fd);
    
    LOG_ERROR("cannot read shared memory fd from slow-path");
    goto close_sockfd;
  }

  u = malloc(sizeof(struct udp_lib));
  if (u == NULL)
  {
    LOG_ERROR("failed to allocate udp_lib struct");
    goto close_sockfd;
  }

  /* Set table to 0 so default fd is SOCK_INACTIVE */
  memset(u->socks, 0, sizeof(struct udp_socket_lib) * MAX_SOCKETS);

  u->uxsocket_fd = sock_fd;
  u->shm_fd = shm_fd;
  u->shm_base = NULL;
  u->next_ctxid = 0;
  u->next_sockfd = 0;
  udp = u;
  
  return 0;

close_sockfd:
  close(sock_fd);
  return -1;
}

struct udp_context_lib * udp_ctx_new()
{
  int i;
  ssize_t sz, off;
  void *shm_base;
  struct udp_context_lib *ctx;
  struct udp_queue_new_actx_res *res;
  __u8 resp_buf[sizeof(*res)];
  struct equeue *eq, **eq_list;
  struct dqueue *dq, **dq_list;
  struct udp_queue_new_actx_req req = {
    .req = 1,
  };
  
  /* Send request on kernel socket */
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

  sz = sendmsg(udp->uxsocket_fd, &msg, 0);
  if (sz != sizeof(req))
  {
    LOG_ERROR("failed to send msg to register udp app ctx");
    perror("");
    return NULL;
  }

  /* Receive response on kernel socket */
  res = (struct udp_queue_new_actx_res *) resp_buf;
  off = 0;
  while (off < sizeof(*res)) 
  {
    sz = read(udp->uxsocket_fd, (__u8 *) res + off, sizeof(*res) - off);
    if (sz < 0) 
    {
      LOG_ERROR("read failed");
      perror("");
      return NULL;
    }
    off += sz;
  }
  
  ctx = malloc(sizeof(struct udp_context_lib));
  if (ctx == NULL)
  {
    LOG_ERROR("failed to allocate udp context struct");
    return NULL;
  }
  
  ctx->id = __sync_fetch_and_add(&udp->next_ctxid, 1);
  ctx->ncores = res->n_fp_cores;
  
  /* Map shared memory if this is the first context */
  if (ctx->id == 0)
  {
    shm_base = mmap(NULL, res->shm_len, PROT_READ | PROT_WRITE, 
        MAP_SHARED | MAP_POPULATE, udp->shm_fd, res->shm_off);
    if (shm_base == (void *) -1) 
    {
      LOG_ERROR("failed to map shm region");
      return NULL;;
    }
    udp->shm_base = shm_base;
  }
  
  /* Wait until shm_base is mapped */
  while (udp->shm_base == NULL) {}

  /* Set queue from app to slow-path */
  eq = equeue_new(res->as_nelems, res->as_elsize,
      udp->shm_base + res->as_off, res->as_off);
  if (eq == NULL)
  {
    LOG_ERROR("failed to create queue from app to slow-path");
    return NULL;
  }
  ctx->app_slow_q = eq;
  
  /* Set queue from slow-path to app */
  dq = dqueue_new(res->sa_nelems, 
      res->sa_elsize,udp->shm_base + res->sa_off, res->sa_off);
  if (dq == NULL)
  {
    LOG_ERROR("failed to create queue from slow-path to app");
    return NULL;
  }
  ctx->slow_app_q = dq;

  /* Allocate list for bump queues between app and fast-path */
  eq_list = malloc(sizeof(struct equeue *) * res->n_fp_cores);
  if (eq_list == NULL)
  {
    LOG_ERROR("failed to allocate list for queues app->fast");
    return NULL;
  }
  ctx->app_fast_qs = eq_list;

  dq_list = malloc(sizeof(struct dqueue *) * res->n_fp_cores);
  if (dq_list == NULL)
  {
    LOG_ERROR("failed to allcoate list for queues fast->app");
    return NULL;
  }
  ctx->fast_app_qs = dq_list;

  /* Create each queue between app and fast-path core */
  for (i = 0; i < res->n_fp_cores; i++)
  {
    eq = equeue_new(res->af_nelems, res->af_elsize,
        udp->shm_base + res->af_offs[i], res->af_offs[i]);
    if (eq == NULL)
    {
      LOG_ERROR("failed to create app->fast bump queue");
      return NULL;
    }
    eq_list[i] = eq;

    dq = dqueue_new(res->fa_nelems, res->fa_elsize,
        udp->shm_base + res->fa_offs[i], res->fa_offs[i]);
    if (dq == NULL)
    {
      LOG_ERROR("failed to create fast->app bump queue");
      return NULL;
    }
    dq_list[i] = dq;
  }

  return ctx;
}

int udp_socket(struct udp_context_lib *ctx)
{
  int fd, ret;
  struct udp_socket_lib *sock;
  struct equeue *q;
  struct udp_queue_entry *qe;
  struct udp_queue_new_sock_req *req;

  if (ctx == NULL)
  {
    LOG_ERROR("passed NULL udp_context_lib");
    return -1;
  }
  
  /* Find next free fd and get socket */
  fd = __sync_fetch_and_add(&udp->next_sockfd, 1);
  if (fd >= MAX_SOCKETS)
  {
    LOG_ERROR("exceeded max nuber of sockets");
    return -1;
  }
  sock = &udp->socks[fd];
  sock->fd = fd;
  sock->rx_avail = 0;
  sock->rx_head = 0;
  sock->tx_avail = 0;
  sock->tx_head = 0;
  sock->tx_port = 0;
  sock->tx_ip = 0;
  sock->rx_port = 0;
  sock->rx_ip = 0;
  sock->bind_success = -1;
  sock->setopt_success = -1;
  
  /* Set to 0 here so we can check if it was initialised when polling slow-path */
  sock->rx_len = 0;

  /* Create socket in slow-path */
  q = ctx->app_slow_q;
  qe = queue_tail(q);
  if (qe == NULL)
  {
    LOG_ERROR("failed to get queue tail");
    return -1;
  }

  req = &qe->data.new_sock_req;
  req->opaque = (__u64) sock;
  ret = queue_enqueue(q, UDP_QUEUE_NEW_SOCK_REQ);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue new sock req");
    return -1;
  }

  /* Wait poll for response */
  while (sock->rx_len == 0)
    udp_poll_slow(ctx);

  return sock->fd;
}

int udp_bind(struct udp_context_lib *ctx, int sockfd, 
    const struct sockaddr *addr, socklen_t addrlen)
{
  int ret;
  struct equeue *q;
  struct udp_queue_entry *qe;
  struct udp_queue_bind_req *req;
  struct sockaddr_in *sin = (struct sockaddr_in *) addr;
  struct udp_socket_lib *sock;
  
  /* Send src ip and port to slow-path */
  q = ctx->app_slow_q;
  qe = queue_tail(q);
  if (qe == NULL)
  {
    LOG_ERROR("failed to get queue tail");
    return -1;
  }
  
  req = &qe->data.bind_req;
  
  sock = &udp->socks[sockfd];
  req->sock_id = sock->sock_id;
  req->local_ip = ntohl(sin->sin_addr.s_addr);
  req->local_port = ntohs(sin->sin_port);
  req->opaque = (__u64) sock;
  ret = queue_enqueue(q, UDP_QUEUE_BIND_REQ);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue bind req");
    return -1;
  }

  while (sock->bind_success == -1)
    udp_poll_slow(ctx);

  if (!sock->bind_success)
    return -1;

  sock->tx_port = sin->sin_port;
  sock->tx_ip = sin->sin_addr.s_addr;
  sock->bind_success = -1;
  return 0;
}

int udp_setsockopt(struct udp_context_lib *ctx, int sockfd, __u8 opt)
{
  int ret;
  struct equeue *q;
  struct udp_queue_entry *qe;
  struct udp_queue_setopt_req *req;
  struct udp_socket_lib *sock;
  
  q = ctx->app_slow_q;
  qe = queue_tail(q);
  if (qe == NULL)
  {
    LOG_ERROR("failed to get queue tail");
    return -1;
  }
  
  sock = &udp->socks[sockfd];
  req = &qe->data.setopt_req;
  req->opt = opt;
  req->sock_id = sockfd;
  req->opaque = (__u64) sock;
  
  ret = queue_enqueue(q, UDP_QUEUE_SETOPT_REQ);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue setsockopt req");
    return -1;
  }
  
  while (sock->setopt_success == -1)
    udp_poll_slow(ctx);
    
  if (!sock->setopt_success)
    return -1;
    
  sock->setopt_success = -1;
  return 0;
}

int udp_sendto(struct udp_context_lib *ctx, int sockfd, 
    const void *buf, size_t len, 
    const struct sockaddr *addr, socklen_t addrlen)
{
  int ret;
  __u32 tail, n, n1, n2;
  __u32 tx_len, tx_avail, tx_head;
  __u32 tx_ip;
  __u16 tx_port;
  __u8 *tx_buf;
  const __u8 *src;
  struct udp_socket_lib *sock;
  struct equeue *q;
  struct udp_queue_bump_entry *qe;
  struct udp_queue_bump_cham_tx *bump;
  struct sockaddr_in *sin = (struct sockaddr_in *) addr;

  sock = &udp->socks[sockfd];
  if (sock->fd == SOCK_INACTIVE)
  {
    LOG_ERROR("bad socket file descriptor");
    return -1;
  }

  if (len == 0)
    return 0;

  tx_len = sock->tx_len;
  tx_avail = sock->tx_avail;
  tx_head = sock->tx_head;
  tx_buf = sock->tx_buf;
  tail = tx_head + tx_avail;
  src = buf;

  /* Prefetch txbuf */
  utils_prefetch0(tx_buf + tail);
  
  /* Not enough space available in buffer */
  if (len > tx_len - tx_avail)
  {
    errno = EAGAIN;
    return -1;
  }
  n = len;

  /* Send bump message to update TX available */
  q = ctx->app_fast_qs[sock->core];
  qe = queue_tail(q);
  if (qe == NULL)
  {
    errno = EAGAIN;
    return -1;
  }

  if (tail >= tx_len)
    tail -= tx_len;

  bump = &qe->data.bump_cham_tx;
  bump->sock_id = sock->sock_id;
  tx_ip = ntohl(sin->sin_addr.s_addr);
  tx_port = ntohs(sin->sin_port);
  bump->tx_ip = tx_ip;
  bump->tx_port = tx_port;
  bump->tx_avail = n;

  /* Only do copy here so prefetch has time to go through */
  if (tail + n > tx_len)
  {
    /* Split copy */
    n1 = tx_len - tail;
    n2 = n - n1;
    memcpy(tx_buf + tail, src, n1);
    memcpy(tx_buf, src + n1, n2);
  }
  else
  {
    /* Simple copy */
    memcpy(tx_buf + tail, src, n);
  }

  sock->tx_avail = tx_avail + n;
  sock->tx_ip = tx_ip;
  sock->tx_port = tx_port;

  ret = queue_enqueue(q, UDP_QUEUE_BUMP_CHAM_TX);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue bump");
    sock->tx_avail = tx_avail;
    return -1;
  }

  return n;
}

int udp_recvfrom(struct udp_context_lib *ctx, int sockfd, 
    void *buf, size_t len, 
    struct sockaddr *addr, socklen_t addr_len)
{
  int n, ret;
  __u32 n1, n2, new_head;
  __u32 rx_len, rx_avail, rx_head;
  __u8 *rx_buf;
  struct equeue *q;
  struct udp_queue_bump_entry *qe;
  struct udp_queue_bump_cham_rx *bump; 
  struct udp_socket_lib *sock;
  struct sockaddr_in *sin = (struct sockaddr_in *) addr;

  sock = &udp->socks[sockfd];
  if (sock->fd == SOCK_INACTIVE)
  {
    LOG_ERROR("bad socket file descriptor");
    return -1;
  }

  /* Copy to buffer */
  rx_len = sock->rx_len;
  rx_avail = sock->rx_avail;
  rx_head = sock->rx_head;
  rx_buf = sock->rx_buf;

  n = len;
  if (n > rx_avail)
    n = rx_avail;
    
  /* There is nothing available */
  if (n == 0)
    return 0;
  
  if (addr != NULL)
  {
    sin->sin_addr.s_addr = htonl(sock->rx_ip);
    sin->sin_port = htons(sock->rx_port);
  }

  /* Send bump message to update RX head */
  q = ctx->app_fast_qs[sock->core];
  qe = queue_tail(q);
  if (qe == NULL)
  {
    errno = EAGAIN;
    return -1;
  }

  bump = &qe->data.bump_cham_rx;
  bump->sock_id = sock->sock_id;
  bump->rx_head = n;

  new_head = rx_head + n;
  if (new_head >= rx_len)
  new_head -= rx_len;
  assert(rx_head >= 0);
  
  /* Only do copy here so prefetch has time to go through */
  if (rx_head + n > rx_len)
  {
    /* Split copy */
    n1 = rx_len - rx_head;
    n2 = n - n1;
    memcpy(buf, rx_buf + rx_head, n1);
    memcpy(buf + n1, rx_buf, n2);
  }
  else
  {
    /* Simple copy */
    memcpy(buf, rx_buf + rx_head, n);
  }

  sock->rx_avail = rx_avail - n;
  sock->rx_head = new_head;
  
  ret = queue_enqueue(q, UDP_QUEUE_BUMP_CHAM_RX);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue bump");
    return -1;
  }
  
  return n;
}

int udp_poll_fast(struct udp_context_lib *ctx)
{
  int i, n, ncores;
  struct dqueue *q;
  struct udp_queue_bump_entry *qe;
  struct dqueue **fast_app_qs;
  
  
  /* Poll for messages from each fast-path core */
  n = 0;
  ncores = ctx->ncores;
  fast_app_qs = ctx->fast_app_qs;
  for (i = 0; i < ncores && n < LIB_BATCH_SIZE; i++)
  {
    q = fast_app_qs[i];
    while (n < LIB_BATCH_SIZE && (qe = queue_head(q)) != NULL)
    {       
      __u32 next_head = q->head + q->elsize;
      if (next_head >= (q->elsize * q->nelems))
        next_head = 0;
      utils_prefetch0((__u8 *) q->entries + next_head);

      n++;
    
      switch (qe->type)
      {
        case UDP_QUEUE_BUMP_APP_TX:
          handle_tx_bump(qe);
          break;
        case UDP_QUEUE_BUMP_APP_RX:
          handle_rx_bump(qe);
          break;
        default:
          LOG_ERROR("unknown queue entry type from "
              "fast-path to app type=%d", qe->type);
          abort();
      }
      
      queue_dequeue(q);      
    }
  }

  return n;
}

int udp_poll_slow(struct udp_context_lib *ctx)
{
  int n;
  struct dqueue *q;
  struct udp_queue_entry *qe;

  n = 0;  
  q = ctx->slow_app_q;
  while (n < LIB_BATCH_SIZE)
  {
    qe = queue_head(q);
  
    /* Queue is empty */
    if (qe == NULL)
      return -1;
      
    n++;
  
    switch (qe->type)
    {
      case UDP_QUEUE_NEW_SOCK_RES:
        handle_new_sock_res(qe);
        break;
      case UDP_QUEUE_BIND_RES:
        handle_bind_res(qe);
        break;
      case UDP_QUEUE_SETOPT_RES:
        handle_setopt_res(qe);
        break;
      default:
        LOG_ERROR("unknown queue entry type from "
            "slow-path to app type=%d", qe->type);
        abort();
    }
    queue_dequeue(q);
    
  }

  return n;  
}

static int handle_new_sock_res(struct udp_queue_entry *qe)
{
  struct udp_queue_new_sock_res *res;
  struct udp_socket_lib *sock;

  res = &qe->data.new_sock_res;
  sock = (struct udp_socket_lib *) res->opaque;
  sock->core = res->core;
  sock->sock_id = res->sock_id;
  sock->rx_qid = res->rx_qid;
  sock->rx_len = res->rx_len;
  sock->rx_buf = udp->shm_base + res->rx_off;
  sock->tx_qid = res->tx_qid;
  sock->tx_len = res->tx_len;
  sock->tx_buf = udp->shm_base +
  res->tx_off;

  return 0;  
}

static int handle_bind_res(struct udp_queue_entry *qe)
{
  struct udp_queue_bind_res *res;
  struct udp_socket_lib *sock;

  res = &qe->data.bind_res;
  sock = (struct udp_socket_lib *) res->opaque;
  sock->bind_success = res->success;

  return 0;
}

static int handle_setopt_res(struct udp_queue_entry *qe)
{
  struct udp_queue_setopt_res *res;
  struct udp_socket_lib *sock;

  res = &qe->data.setopt_res;
  sock = (struct udp_socket_lib *) res->opaque;
  sock->setopt_success = res->success;

  return 0;
}

static int handle_tx_bump(struct udp_queue_bump_entry *qe)
{
  __u32 new_head;
  struct udp_queue_bump_app_tx *bump;
  struct udp_socket_lib *sock;

  bump = &qe->data.bump_app_tx;
  sock = (struct udp_socket_lib *) bump->opaque;

  new_head = sock->tx_head + bump->tx_head;
  if (new_head >= sock->tx_len)
    new_head -= sock->tx_len;
  sock->tx_head = new_head;
  sock->tx_avail -= bump->tx_head;

  return 0;  
}

static int handle_rx_bump(struct udp_queue_bump_entry *qe)
{
  struct udp_queue_bump_app_rx *bump;
  struct udp_socket_lib *sock;

  bump = &qe->data.bump_app_rx;
  sock = (struct udp_socket_lib *) bump->opaque;
  utils_prefetch0(sock->rx_buf + sock->rx_head);
  sock->rx_avail += bump->rx_avail;
  sock->rx_port = bump->rx_port;
  sock->rx_ip = bump->rx_ip;

  return 0;  
}

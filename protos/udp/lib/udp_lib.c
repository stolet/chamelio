#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/mman.h>

#include <cham_lib.h>

#include "udp_lib.h"
#include "udp_queue.h"
#include "log.h"
#include "uxsocket.h"

static struct udp_lib *udp = NULL;
/* One context per thread */
static __thread struct udp_context_lib *udp_ctx = NULL;

int handle_new_sock_res(struct udp_queue_entry *qe);

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
  memset(u->socks, 0, sizeof(struct udp_socket) * MAX_SOCKETS);

  u->uxsocket_fd = sock_fd;
  u->shm_fd = shm_fd;
  u->shm_base = NULL;
  u->next_ctxid = 0;
  udp = u;
  
  return 0;

close_sockfd:
  close(sock_fd);
  return -1;
}

int udp_ctx_new()
{
  int i;
  ssize_t sz, off;
  void *shm_base;
  struct udp_context_lib *ctx;
  struct udp_queue_new_actx_res *res;
  uint8_t resp_buf[sizeof(*res)];
  struct equeue *eq, **eq_list;
  struct dqueue *dq, **dq_list;
  struct udp_queue_new_actx_req req = {
    .req = 1,
  };
  
  /* Context was already initialised in this thread */
  if (udp_ctx != NULL)
  {
    LOG_WARN("context was already initialised in this thread");
    return -1;
  }
  
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
    return -1;
  }

  /* Receive response on kernel socket */
  res = (struct udp_queue_new_actx_res *) resp_buf;
  off = 0;
  while (off < sizeof(*res)) 
  {
    sz = read(udp->uxsocket_fd, (uint8_t *) res + off, sizeof(*res) - off);
    if (sz < 0) 
    {
      LOG_ERROR("read failed");
      perror("");
      return -1;
    }
    off += sz;
  }
  
  ctx = malloc(sizeof(struct udp_context_lib));
  if (ctx == NULL)
  {
    LOG_ERROR("failed to allocate udp context struct");
    return -1;
  }
  
  ctx->id = __sync_fetch_and_add(&udp->next_ctxid, 1);
  ctx->ncores = res->n_fp_cores;
  udp_ctx = ctx;
  
  /* Map shared memory if this is the first context */
  if (ctx->id == 0)
  {
    shm_base = mmap(NULL, res->shm_len, PROT_READ | PROT_WRITE, 
        MAP_SHARED | MAP_POPULATE, udp->shm_fd, 0);
    if (shm_base == (void *) -1) 
    {
      LOG_ERROR("failed to map shm region");
      return -1;;
    }
    udp->shm_base = shm_base;
  }
  
  /* Wait until shm_base is mapped */
  while (udp->shm_base == NULL) {}

  /* Set queue from app to slow-path */
  eq = equeue_new(res->as_len, udp->shm_base + res->as_off, res->as_off);
  if (eq == NULL)
  {
    LOG_ERROR("failed to create queue from app to slow-path");
    return -1;
  }
  ctx->app_slow_q = eq;
  
  /* Set queue from slow-path to app */
  dq = dqueue_new(res->sa_len, udp->shm_base + res->sa_off, res->sa_off);
  if (dq == NULL)
  {
    LOG_ERROR("failed to create queue from slow-path to app");
    return -1;
  }
  ctx->slow_app_q = dq;

  /* Allocate list for bump queues between app and fast-path */
  eq_list = malloc(sizeof(struct equeue *) * res->n_fp_cores);
  if (eq_list == NULL)
  {
    LOG_ERROR("failed to allocate list for queues app->fast");
    return -1;
  }
  udp_ctx->app_fast_qs = eq_list;

  dq_list = malloc(sizeof(struct dqueue *) * res->n_fp_cores);
  if (dq == NULL)
  {
    LOG_ERROR("failed to allcoate list for queues fast->app");
    return -1;
  }
  udp_ctx->fast_app_qs = dq_list;

  /* Create each queue between app and fast-path */
  for (i = 0; i < res->n_fp_cores; i++)
  {
    eq = equeue_new(res->af_len, 
        udp->shm_base + res->af_offs[i], res->af_offs[i]);
    if (eq == NULL)
    {
      LOG_ERROR("failed to create app->fast bump queue");
      return -1;
    }
    eq_list[i] = eq;

    dq = dqueue_new(res->fa_len,
        udp->shm_base + res->fa_offs[i], res->fa_offs[i]);
    if (dq == NULL)
    {
      LOG_ERROR("failed to create fast->app bump queue");
      return -1;
    }
    dq_list[i] = dq;
  }

  return 0;
}

int udp_socket()
{
  int fd, ret;
  struct udp_socket *sock;
  struct equeue *q;
  struct udp_queue_entry *qe;
  struct udp_queue_new_sock_req *req;

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

  /* Create socket in slow-path */
  assert(udp_ctx != NULL);
  q = udp_ctx->app_slow_q;
  qe = udp_queue_tail(q);
  if (qe == NULL)
  {
    LOG_ERROR("failed to get queue tail");
    return -1;
  }

  req = &qe->data.new_sock_req;
  req->opaque = (uint64_t) sock;
  ret = udp_queue_enqueue(q, UDP_QUEUE_NEW_SOCK_REQ);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue new sock req");
    return -1;
  }

  /* Wait poll for response */
  while (udp_poll_slow() != UDP_QUEUE_NEW_SOCK_RES) {}

  return 0;
}

int udp_sendto(int sockfd, const void *buf, size_t len, 
    const struct sockaddr *addr, socklen_t addrlen)
{
  int n, ret;
  uint32_t ip, tail, n1, n2;
  uint16_t port;
  struct udp_socket *sock;
  struct equeue *q;
  struct udp_queue_entry *qe;
  struct udp_queue_bump *bump;
  struct sockaddr_in *sin = (struct sockaddr_in *) addr;

  sock = &udp->socks[sockfd];
  if (sock->fd == SOCK_INACTIVE)
  {
    LOG_ERROR("bad socket file descriptor");
    return -1;
  }

  ip = ntohl(sin->sin_addr.s_addr);
  port = ntohs(sin->sin_port);

  n = len;
  if (len > sock->tx_len - sock->tx_avail)
    n = sock->tx_len - sock->tx_avail;

  /* No space available in tx buffer */
  if (n == 0)
    return 0;

  tail = sock->tx_head + sock->tx_avail;
  if (sock->tx_head + sock->tx_avail > sock->tx_len)
    tail = sock->tx_head + sock->tx_avail - sock->tx_len;

  if (tail + n > sock->tx_len)
  {
    /* Split copy */
    n1 = sock->tx_len - tail;
    n2 = n - n1;
    memcpy(sock->tx_buf + tail, buf, n1);
    memcpy(sock->tx_buf, buf + n1, n2);
  }
  else
  {
    /* Simple copy */
    memcpy(sock->tx_buf + tail, buf, n);
  }

  sock->tx_avail = sock->tx_avail + n;
    
  /* Send bump message to update TX available */
  q = udp_ctx->app_fast_qs[sock->core];
  qe = udp_queue_tail(q);
  if (qe == NULL)
  {
    LOG_ERROR("failed to get queue tail");
    return -1;
  }

  bump = &qe->data.bump;
  bump->tx_head = 0;
  bump->tx_avail = n;
  bump->rx_head = 0;
  bump->rx_avail = 0;

  ret = queue_enqueue(q, UDP_QUEUE_BUMP);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue bump");
    return -1;
  }

  return n;
}

int udp_recvfrom(int sockfd, void *buf, size_t len, 
    struct sockaddr *addr, socklen_t addr_len)
{
  int n;
  uint32_t ip, n1, n2;
  uint16_t port;
  struct equeue *q;
  struct udp_queue_entry *qe;
  struct udp_queue_bump *bump; 
  struct udp_socket *sock;
  struct sockaddr_in *sin = (struct sockaddr_in *) addr;

  sock = &udp->socks[sockfd];
  if (sock->fd == SOCK_INACTIVE)
  {
    LOG_ERROR("bad socket file descriptor");
    return -1;
  }

  ip = ntohl(sin->sin_addr.s_addr);
  port = ntohs(sin->sin_port);

  /* Copy to buffer */
  n = len;
  if (len > sock->rx_avail)
    n = sock->rx_avail;

  if (sock->rx_head + sock->rx_avail > sock->rx_len)
  {
    /* Split copy */
    n1 = sock->rx_len - sock->rx_head;
    n2 = n - n1;
    memcpy(buf, sock->rx_buf + sock->rx_head, n1);
    memcpy(buf + n1, sock->rx_buf, n2);
  }
  else
  {
    /* Simple copy */
    memcpy(buf, sock->rx_buf + sock->rx_head, n);
  }
  
  sock->rx_avail -= n;
  sock->rx_head = sock->rx_head + n;
  if (sock->rx_head > sock->rx_len)
    sock->rx_head = sock->rx_head - sock->rx_len;

  /* Send bump message to update RX head */
  q = udp_ctx->app_fast_qs[sock->core];
  qe = udp_queue_tail(q);
  if (qe == NULL)
  {
    LOG_ERROR("failed to get queue tail");
    return -1;
  }

  bump = &qe->data.bump;
  bump->rx_head = n;
  bump->rx_avail = 0;
  bump->tx_avail = 0;
  bump->tx_head = 0;

  return n;
}

int udp_poll_slow()
{
  struct dqueue *q;
  struct udp_queue_entry *qe;

  q = udp_ctx->slow_app_q;
  qe = udp_queue_head(q);

  /* Queue is empty */
  if (qe == NULL)
    return -1;

  switch (qe->type)
  {
    case UDP_QUEUE_NEW_SOCK_RES:
      handle_new_sock_res(qe);
      queue_dequeue(q);
      return UDP_QUEUE_NEW_SOCK_RES;
    default:
      LOG_ERROR("unknown queue entry type from "
          "slow-path to app type=%d", qe->type);
      abort();
  }

  return 0;  
}

int handle_new_sock_res(struct udp_queue_entry *qe)
{
  struct udp_queue_new_sock_res *res;
  struct udp_socket *sock;

  res = &qe->data.new_sock_res;
  sock = (struct udp_socket *) res->opaque;
  sock->core = res->core;
  sock->rx_qid = res->rx_qid;
  sock->rx_len = res->rx_len;
  sock->rx_buf = udp->shm_base + res->rx_off;
  sock->tx_qid = res->tx_qid;
  sock->tx_len = res->tx_len;
  sock->tx_buf = udp->shm_base + res->tx_off;

  return 0;  
}
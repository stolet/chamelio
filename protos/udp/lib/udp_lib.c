#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <errno.h>

#include <cham_lib.h>

#include "udp_lib.h"
#include "queue.h"
#include "udp_queue.h"
#include "log.h"
#include "uxsocket.h"

#define POLL_BATCH 16

static struct udp_lib *udp = NULL;

/* One context per thread */
static __thread struct udp_context_lib *udp_ctx = NULL;

static int handle_new_sock_res(struct udp_queue_entry *qe);
static int handle_bump(struct udp_queue_entry *qe);

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
  u->next_sockfd = 0;
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
  eq = equeue_new(res->as_nelems, res->as_elsize,
      udp->shm_base + res->as_off, res->as_off);
  if (eq == NULL)
  {
    LOG_ERROR("failed to create queue from app to slow-path");
    return -1;
  }
  ctx->app_slow_q = eq;
  
  /* Set queue from slow-path to app */
  dq = dqueue_new(res->sa_nelems, res->sa_elsize,udp->shm_base + res->sa_off, res->sa_off);
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
    eq = equeue_new(res->af_nelems, res->af_elsize,
        udp->shm_base + res->af_offs[i], res->af_offs[i]);
    if (eq == NULL)
    {
      LOG_ERROR("failed to create app->fast bump queue");
      return -1;
    }
    eq_list[i] = eq;

    dq = dqueue_new(res->fa_nelems, res->fa_elsize,
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
  
  /* Set to 0 here so we can check it was initialised when polling slow-path */
  sock->rx_len = 0;

  /* Create socket in slow-path */
  assert(udp_ctx != NULL);
  q = udp_ctx->app_slow_q;
  qe = queue_tail(q);
  if (qe == NULL)
  {
    LOG_ERROR("failed to get queue tail");
    return -1;
  }

  req = &qe->data.new_sock_req;
  req->opaque = (uint64_t) sock;
  ret = queue_enqueue(q, UDP_QUEUE_NEW_SOCK_REQ);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue new sock req");
    return -1;
  }

  /* Wait poll for response */
  while (sock->rx_len == 0)
    udp_poll_slow();

  return sock->fd;
}

int udp_bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
  int ret;
  struct equeue *q;
  struct udp_queue_entry *qe;
  struct udp_queue_bind *req;
  struct sockaddr_in *sin = (struct sockaddr_in *) addr;
  
  /* Send src ip and port to slow-path */
  q = udp_ctx->app_slow_q;
  qe = queue_tail(q);
  if (qe == NULL)
  {
    LOG_ERROR("failed to get queue tail");
    return -1;
  }
  
  req = &qe->data.bind;
  req->sock_id = udp->socks[sockfd].sock_id;
  req->src_ip = sin->sin_addr.s_addr;
  req->src_port = sin->sin_port;
  ret = queue_enqueue(q, UDP_QUEUE_BIND);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue new sock req");
    return -1;
  }

  return 0;
}

int udp_sendto(int sockfd, const void *buf, size_t len, 
    const struct sockaddr *addr, socklen_t addrlen)
{
  int n, ret;
  uint32_t tail, n1, n2;
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

  n = len;
  if (len > sock->tx_len - sock->tx_avail)
    n = sock->tx_len - sock->tx_avail;

  /* No space available in tx buffer */
  if (n == 0)
  {
    errno = EAGAIN;
    return -1;
  }

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
  qe = queue_tail(q);
  if (qe == NULL)
  {
    LOG_ERROR("failed to get queue tail");
    return -1;
  }

  bump = &qe->data.bump;
  bump->sock_id = sock->sock_id;
  bump->dst_ip = sin->sin_addr.s_addr;
  bump->dst_port = sin->sin_port;
  bump->opaque = (uint64_t) sock;
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
  int n, ret;
  uint32_t n1, n2, new_head;
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

  /* Copy to buffer */
  n = len;
  if (len > sock->rx_avail)
    n = sock->rx_avail;
    
  /* There is nothing available */
  if (n == 0)
  {
    errno = EAGAIN;
    return -1;
  }
    
  assert(sock->rx_head >= 0);
  if (sock->rx_head + n > sock->rx_len)
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
  new_head = sock->rx_head + n;
  if (new_head > sock->rx_len)
    new_head -= sock->rx_len;
  sock->rx_head = new_head;

  /* Send bump message to update RX head */
  q = udp_ctx->app_fast_qs[sock->core];
  qe = queue_tail(q);
  if (qe == NULL)
  {
    LOG_ERROR("failed to get queue tail");
    return -1;
  }

  bump = &qe->data.bump;
  bump->sock_id = sock->sock_id;
  bump->opaque = (uint64_t) sock;
  bump->rx_head = n;
  bump->rx_avail = 0;
  bump->tx_avail = 0;
  bump->tx_head = 0;
  if (addr != NULL)
  {
    bump->dst_ip = sin->sin_addr.s_addr;
    bump->dst_port = sin->sin_port; 
  }
  
  ret = queue_enqueue(q, UDP_QUEUE_BUMP);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue bump");
    return -1;
  }
  
  return n;
}

int udp_poll_fast()
{
  int i, n;
  struct dqueue *q;
  struct udp_queue_entry *qe;

  /* Poll for messages from each fast-path core */
  n = 0;
  for (i = 0; i < udp_ctx->ncores && n < POLL_BATCH; i++)
  {
    q = udp_ctx->fast_app_qs[i];
    while (n < POLL_BATCH)
    {
      qe = queue_head(q);
    
      /* Queue is empty */
      if (qe == NULL)
        break;;
        
      n++;
    
      switch (qe->type)
      {
        case UDP_QUEUE_BUMP:
          handle_bump(qe);
          queue_dequeue(q);
          break;
        default:
          LOG_ERROR("unknown queue entry type from "
              "fast-path to app type=%d", qe->type);
          abort();
      }
    }
  }

  return 0;
}

int udp_poll_slow()
{
  int n;
  struct dqueue *q;
  struct udp_queue_entry *qe;

  
  n = 0;
  
  q = udp_ctx->slow_app_q;
  while (n < POLL_BATCH)
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
        queue_dequeue(q);
        break;
      case UDP_QUEUE_BUMP:
        handle_bump(qe);
        queue_dequeue(q);
        break;
      default:
        LOG_ERROR("unknown queue entry type from "
            "slow-path to app type=%d", qe->type);
        abort();
    }
  }

  return 0;  
}

static int handle_new_sock_res(struct udp_queue_entry *qe)
{
  struct udp_queue_new_sock_res *res;
  struct udp_socket *sock;

  res = &qe->data.new_sock_res;
  sock = (struct udp_socket *) res->opaque;
  sock->core = res->core;
  sock->sock_id = sock->sock_id;
  sock->rx_qid = res->rx_qid;
  sock->rx_len = res->rx_len;
  sock->rx_buf = udp->shm_base + res->rx_off;
  sock->tx_qid = res->tx_qid;
  sock->tx_len = res->tx_len;
  sock->tx_buf = udp->shm_base + res->tx_off;

  return 0;  
}

static int handle_bump(struct udp_queue_entry *qe)
{
  uint32_t new_head;
  struct udp_queue_bump *bump;
  struct udp_socket *sock;

  bump = &qe->data.bump;
  sock = (struct udp_socket *) bump->opaque;

  sock->rx_avail += bump->rx_avail;
  new_head = sock->tx_head + bump->tx_head;
  if (new_head > sock->tx_len)
    new_head -= sock->tx_len;
  sock->tx_head = new_head;
  sock->tx_avail -= bump->tx_head;

  return 0;  
}
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <sys/mman.h>

#include <cham_lib.h>

#include "udp_lib.h"
#include "udp_queue.h"
#include "log.h"


static int nctxs = 0;

/* Adter udp_connect_slow this becomes read only*/
static struct udp_lib *udp = NULL;

/* One context per thread */
static __thread struct udp_context_lib *udp_ctx = NULL;

static int uxsocket_read_one_msg(int sock_fd, int64_t *index, int *fd);

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

  u->uxsocket_fd = sock_fd;
  u->shm_fd = shm_fd;
  u->shm_base = NULL;
  udp = u;
  
  return 0;

close_sockfd:
  close(sock_fd);
  return -1;
}

int udp_ctx_new()
{
  ssize_t sz, off;
  void *shm_base;
  struct udp_context_lib *ctx;
  struct udp_queue_new_actx_res *res;
  uint8_t resp_buf[sizeof(*res)];
  struct equeue *eq;
  struct dqueue *dq;
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
  
  ctx->id = __sync_fetch_and_add(&nctxs, 1);
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
  

  return 0;
}

int udp_socket()
{
  return 0;
}

static int uxsocket_read_one_msg(int sock_fd, int64_t *index, int *fd)
{
    int ret;
    struct msghdr msg;
    struct iovec iov[1];
    union {
        struct cmsghdr cmsg;
        char control[CMSG_SPACE(sizeof(int))];
    } msg_control;
    struct cmsghdr *cmsg;

    iov[0].iov_base = index;
    iov[0].iov_len = sizeof(*index);

    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;
    msg.msg_control = &msg_control;
    msg.msg_controllen = sizeof(msg_control);

    ret = recvmsg(sock_fd, &msg, 0);
    if (ret < sizeof(*index)) 
    {
      LOG_ERROR("cannot read message");
      perror("");
      return -1;
    }

    if (ret == 0) 
    {
      LOG_ERROR("lost connection to server");
      return -1;
    }

    // *index = GINT64_FROM_LE(*index);
    *fd = -1;

    for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) 
    {

      if (cmsg->cmsg_len != CMSG_LEN(sizeof(int)) ||
          cmsg->cmsg_level != SOL_SOCKET ||
          cmsg->cmsg_type != SCM_RIGHTS) 
      {
        continue;
      }

      memcpy(fd, CMSG_DATA(cmsg), sizeof(*fd));
    }

    return 0;
}
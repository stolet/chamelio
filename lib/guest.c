#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <sys/mman.h>

#include "cham_lib.h"
#include "queue.h"
#include "log.h"

static int uxsocket_read_one_msg(int sock_fd, int64_t *index, int *fd);

struct guest_lib * cham_connect_guest()
{
  struct guest_lib *g;
  struct sockaddr_un s_un;
  int shm_fd, ret, sock_fd;
  int64_t tmp;

  sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock_fd < 0) 
  {
    LOG_ERROR("failed to create socket");
    return NULL;
  }

  s_un.sun_family = AF_UNIX;
  ret = snprintf(s_un.sun_path, sizeof(s_un.sun_path), 
      "%s", GUEST_SOCKET_PATH);
  if (ret < 0 || ret >= sizeof(s_un.sun_path)) 
  {
    LOG_ERROR("could not copy unix socket path");
    goto close_sockfd;
  }

  if (connect(sock_fd, (struct sockaddr *)&s_un, sizeof(s_un)) < 0) 
  {
    LOG_ERROR("cannot connect to chamelio, %s", GUEST_SOCKET_PATH);
    perror("");
    goto close_sockfd;
  }

  /* Get shared mem fd */
  if (uxsocket_read_one_msg(sock_fd, &tmp, &shm_fd) < 0 || tmp != -1 || shm_fd < 0) 
  {
    if (shm_fd >= 0) 
      close(shm_fd);
    
    LOG_ERROR("cannot read shared memory fd from chamelio");
    goto close_sockfd;
  }

  g = malloc(sizeof(struct guest_lib));
  if (g == NULL)
  {
    LOG_ERROR("failed to allocate guest_lib struct");
    goto close_sockfd;
  }

  g->uxsocket_fd = sock_fd;
  g->shm_fd = shm_fd;

  return g;

close_sockfd:
  close(sock_fd);
  return NULL;
}

struct proto_lib * cham_new_proto(struct guest_lib *g, uint32_t shmsize)
{
  int ret;
  ssize_t sz, off;
  void *shm_base;
  struct proto_lib *p;
  struct equeue *eq;
  struct dqueue *dq;
  struct queue_new_proto_res *res;
  struct shm_allocator *alloc;
  struct shm_handle *sh;
  uint8_t resp_buf[sizeof(*res)];
  uint8_t ptype = 1;
  struct queue_new_proto_req req = {
    .proto_type = ptype,
  };

  /* Send request on kernel socket */
  struct iovec iov = {
    .iov_base = &req,
    .iov_len = sizeof(req),
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

  struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(int));

  sz = sendmsg(g->uxsocket_fd, &msg, 0);
  assert(sz == sizeof(req));

  /* Receive response on kernel socket */
  res = (struct queue_new_proto_res *) resp_buf;
  off = 0;
  while (off < sizeof(*res)) 
  {
    sz = read(g->uxsocket_fd, (uint8_t *) res + off, sizeof(*res) - off);
    if (sz < 0) 
    {
      LOG_ERROR("read failed");
      perror("");
      return NULL;
    }
    off += sz;
  }

  shm_base = mmap(NULL, res->shm_len, PROT_READ | PROT_WRITE, 
      MAP_SHARED | MAP_POPULATE, g->shm_fd, 0);
  if (shm_base == (void *) -1) 
  {
    LOG_ERROR("failed to map shm region");
    return NULL;
  }

  p = malloc(sizeof(struct proto_lib));
  if (p == NULL)
  {
    LOG_ERROR("failed to allocate proto struct");
    return NULL;
  }
  p->nqueues = 0;
  p->nelems = 0;
  p->elsize = 0;
  p->shm_base = shm_base;
  p->shm_size = res->shm_len;

  /* Create allocator that manages shared memory */
  alloc = shmalloc_init(g->shm_fd, shm_base, res->shm_len);
  if (alloc == NULL)
  {
    LOG_ERROR("failed to create allocator");
    return NULL;
  }

  /* Create queue for messages from guest to the control-path */
  ret = shmalloc_alloc(alloc, CTL_PATH_Q_SZ, &sh);
  if (ret != 0)
  {
    LOG_ERROR("failed to allocate memory for queue");
    return NULL;
  }

  eq = equeue_new(sh->len, sh->addr, sh->off);
  if (eq == NULL)
  {
    LOG_ERROR("failed to create queue");
    return NULL;
  }
  p->guest_ctl_q = eq;

  /* Queue from the guest to the control path should always
     be the first thing allocated in the shared memory region. */
  assert(eq->off == 0);

  /* Create queue for messages from control-path to the guest */
  ret = shmalloc_alloc(alloc, CTL_PATH_Q_SZ, &sh);
  if (ret != 0)
  {
    LOG_ERROR("failed to allocate memory for queue");
    return NULL;
  }

  dq = dqueue_new(sh->len, sh->addr, sh->off);
  if (dq == NULL)
  {
    LOG_ERROR("failed to allocate memory for queue");
    return NULL;
  }
  p->ctl_guest_q = dq;

  /* Queue from the control path to the guest should always be the
     second thing allocated in the shared memory region */
  assert(dq->off == CTL_PATH_Q_SZ);

  return p;
}

int cham_new_queues(struct proto_lib *p, 
    uint16_t nqueues, uint32_t nelems, uint32_t elsize)
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
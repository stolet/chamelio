#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <assert.h>

#include "log.h"
#include "queue.h"
#include "cham_lib.h"

static int uxsocket_read_one_msg(int sock_fd, int64_t *index, int *fd);

int cham_init_guest()
{
  struct sockaddr_un s_un;
  int fd, ret, sock_fd;
  int64_t tmp;

  sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock_fd < 0) 
  {
    LOG_ERROR("Failed to create socket");
    return -1;
  }

  s_un.sun_family = AF_UNIX;
  ret = snprintf(s_un.sun_path, sizeof(s_un.sun_path), 
      "%s", GUEST_SOCKET_PATH);
  if (ret < 0 || ret >= sizeof(s_un.sun_path)) 
  {
    LOG_ERROR("Could not copy unix socket path");
    goto err_close;
  }

  if (connect(sock_fd, (struct sockaddr *)&s_un, sizeof(s_un)) < 0) 
  {
    LOG_ERROR("Cannot connect to chamelio");
    goto err_close;
  }

  /* Read protocol version */
  if (uxsocket_read_one_msg(sock_fd, &tmp, &fd) < 0 || 
      (tmp != IVSHMEM_PROTOCOL_VERSION) || fd != -1) 
  {
    LOG_ERROR("Cannot read protocol version from chamelio");
    goto err_close;
  }

  /* Read guest id */
  if (uxsocket_read_one_msg(sock_fd, &tmp, &fd) < 0 || tmp < 0 || fd != -1) 
  {
    LOG_ERROR("Cannot read index and fd from chamelio");
    goto err_close;
  }

  /* now, we expect shared mem fd */
  if (uxsocket_read_one_msg(sock_fd, &tmp, &fd) < 0 || tmp != -1 || fd < 0) 
  {
    if (fd >= 0) 
      close(fd);
    
    LOG_ERROR("Cannot read shared memory fd from chamelio");
    goto err_close;
  }

  return fd;

err_close:
  close(sock_fd);
  return -1;
}

struct app_lib * cham_init_app()
{
  struct app_lib *a;
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
      "%s", APP_SOCKET_PATH);
  if (ret < 0 || ret >= sizeof(s_un.sun_path)) 
  {
    LOG_ERROR("could not copy unix socket path");
    goto err_close;
  }

  if (connect(sock_fd, (struct sockaddr *)&s_un, sizeof(s_un)) < 0) 
  {
    LOG_ERROR("cannot connect to chamelio, %s", APP_SOCKET_PATH);
    perror("");
    goto err_close;
  }

  /* Get shared mem fd */
  if (uxsocket_read_one_msg(sock_fd, &tmp, &shm_fd) < 0 || tmp != -1 || shm_fd < 0) 
  {
    if (shm_fd >= 0) 
      close(shm_fd);
    
    LOG_ERROR("cannot read shared memory fd from chamelio");
    goto err_close;
  }

  a = malloc(sizeof(struct app_lib));
  if (a == NULL)
  {
    LOG_ERROR("failed to allocate app_lib struct");
    goto err_close;
  }

  a->uxsocket_fd = sock_fd;
  a->shm_fd = shm_fd;

  return a;

err_close:
  close(sock_fd);
  return NULL;
}

/* TODO: Pass enum protocol_type instead of uint8_t */
struct app_context_lib * cham_init_app_ctx(struct app_lib *a, uint8_t proto_type)
{
  int i;
  ssize_t sz, off;
  struct dqueue *app_bump_q, *cham_app_q;
  struct equeue *cham_bump_q, *app_cham_q;
  struct app_context_lib *actx;
  struct queue_new_app_ctx_res *res;
  uint8_t resp_buf[sizeof(*res)];
  struct queue_new_app_ctx_req req = {
    .proto_type = proto_type,
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
  sz = sendmsg(a->uxsocket_fd, &msg, 0);
  assert(sz == sizeof(req));

  /* Receive response on kernel socket */
  res = (struct queue_new_app_ctx_res *) resp_buf;
  off = 0;
  while (off < sizeof(*res)) 
  {
    sz = read(a->uxsocket_fd, (uint8_t *) res + off, sizeof(*res) - off);
    if (sz < 0) 
    {
      LOG_ERROR("read failed");
      perror("");
      return NULL;
    }
    off += sz;
  }

  actx = malloc(sizeof(struct app_context_lib));
  if (actx == NULL)
  {
    LOG_ERROR("failed to allocate app context");
    return NULL;
  }
  actx->id = a->n_ctxs;
  actx->app = a;
  actx->n_fp_cores = res->n_fp_cores;

  /* Create queue for messages from app context to Chamelio */
  app_cham_q = equeue_new(res->app_cham_q_len,
    a->shm_base + res->app_cham_q_off, res->app_cham_q_off);
  if (app_cham_q == NULL)
  {
    LOG_ERROR("failed to create queue from app context to Chamelio");
    goto free_actx;
  }
  actx->app_cham_q = app_cham_q;

  /* Create queue for messages from Chamelio to app context */
  cham_app_q = dqueue_new(res->cham_app_q_len,
    a->shm_base + res->cham_app_q_off, res->cham_app_q_off);
  if (cham_app_q == NULL)
  {
    LOG_ERROR("failed to create queue from Chamelio to app context");
    goto free_actx;
  }
  actx->cham_app_q = cham_app_q;

  /* Create bump queues for each fast-path core */
  for (i = 0; i < res->n_fp_cores; i++)
  {
    app_bump_q = dqueue_new(res->app_bump_q_len, 
        a->shm_base + res->app_bump_q_offs[i], res->app_bump_q_offs[i]);
    if (app_bump_q == NULL)
    {
      LOG_ERROR("failed to create rx bump queue=%d for app ctx=%d", 
          i, actx->id);
      goto free_actx;
    }
    actx->bump_app_q[i] = app_bump_q;

    cham_bump_q = equeue_new(res->cham_bump_q_len,
      a->shm_base + res->cham_bump_q_offs[i], res->cham_bump_q_offs[i]);
    if (cham_bump_q == NULL)
    {
      LOG_ERROR("failed to create tx bump queue=%d for app ctx=%d",
        i, actx->id);
      goto free_actx;
    }
    actx->bump_cham_q[i] = cham_bump_q;
  }

  a->n_ctxs++;
  actx->next = a->ctxs;
  a->ctxs = actx;

  return actx;

free_actx:
  free(actx);
  return NULL;
}

int cham_init_buf(struct app_context_lib *actx)
{
  struct buff_lib *buf;
  struct queue_entry *qe;

  buf = malloc(sizeof(struct buff_lib));
  if (buf == NULL)
  {
    LOG_ERROR("failed to allocate buffer");
    return -1;
  }

  qe = queue_tail(actx->app_cham_q);
  if (qe == NULL)
  {
    LOG_ERROR("failed to get queue tail");
    return -1;
  }

  queue_enqueue(actx->app_cham_q, QUEUE_NEW_BUF);
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
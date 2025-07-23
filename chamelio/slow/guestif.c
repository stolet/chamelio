#include <stdlib.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/un.h>
#include <sys/eventfd.h>
#include <stdio.h>
#include <unistd.h>

#include "shm.h"
#include "guestif.h"
#include "slow.h"
#include "log.h"

#define IVSHMEM_PROTOCOL_VERSION 0
#define HOST_PEERID 255

#define EP_LISTEN_GUEST 1
#define EP_GUEST 2

static int uxsocket_init(struct slow_context *ctx);
static int uxsocket_init_fd(struct slow_context *ctx);
static int uxsocket_accept(struct slow_context *ctx);
static void uxsocket_error(struct slow_context *ctx, struct guest_event *gev);
static int uxsocket_send_int(int fd, int64_t i);
static int uxsocket_sendfd(int uxfd, int fd, int64_t i);

int guestif_init(struct slow_context *ctx)
{
  int ret;

  ret = uxsocket_init(ctx);
  if (ret != 0)
  {
    LOG_ERROR("uxsocket init failed");
    return -1;
  }

  return 0;
}

int guestif_poll(struct slow_context *ctx)
{
  int n, i;
  struct epoll_event evs[32];
  struct guest_event *gev;

  n = epoll_wait(ctx->guest_epfd, evs, 32, 0);

  for (i = 0; i < n; i++)
  {
    gev = evs[i].data.ptr;
    switch (gev->type)
    {
      case EP_LISTEN_GUEST:
        uxsocket_accept(ctx);
        break;
      case EP_GUEST:
        uxsocket_error(ctx, gev);
        break;
      default:
        LOG_WARN("unknown guest event type");
    }
  }

  return n;
}

static int uxsocket_init(struct slow_context *ctx)
{
  int epfd, ret;

  epfd = epoll_create1(0);
  if (epfd == -1)
  {
    LOG_ERROR("epoll_create failed");
    perror("");
    return -1;
  }
  ctx->guest_epfd = epfd;

  ret = uxsocket_init_fd(ctx);
  if (ret != 0)
  {
    LOG_ERROR("failed to init unix socket for guests");
    goto error_close_ep;
  }

  return 0;

error_close_ep:
  close(epfd);

  return -1;
}

static int uxsocket_init_fd(struct slow_context *ctx)
{
  int fd, ret;
  struct epoll_event ev;
  struct sockaddr_un saun;
  struct guest_event *gev;

  fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd == -1) 
  {
    LOG_ERROR("socket creation failed");
    perror("");
    return -1;
  }

  memset(&saun, 0, sizeof(saun));
  saun.sun_family = AF_UNIX;
  memcpy(saun.sun_path, GUEST_SOCKET_PATH, sizeof(GUEST_SOCKET_PATH));
  unlink(saun.sun_path);

  ret = bind(fd, (struct sockaddr *) &saun, sizeof(saun));
  if (ret != 0) 
  {
    LOG_ERROR("bind failed");
    perror("");
    goto error_close;
  }

  ret = listen(fd, 5);
  if (ret != 0) 
  {
    LOG_ERROR("listen failed");
    perror("");
    goto error_close;
  }

  gev = malloc(sizeof(struct guest_event));
  if (gev == NULL)
  {
    LOG_ERROR("failed to malloc guest event");
    perror("");
    goto error_close;
  }

  gev->type = EP_LISTEN_GUEST;

  ev.events = EPOLLIN;
  ev.data.ptr = gev;
  ret = epoll_ctl(ctx->guest_epfd, EPOLL_CTL_ADD, fd, &ev);
  if (ret != 0) {
    LOG_ERROR("epoll_ctl listen failed");
    perror("");
    goto error_free_gev;
  }

  ctx->guest_uxfd = fd;

  return 0;

error_free_gev:
  free(gev);
error_close:
  close(fd);

  return -1;
}

static int uxsocket_accept(struct slow_context *ctx)
{
  int ret, cfd, ifd, nfd, sfd;
  void *shm_base;
  char shm_name[30];
  struct epoll_event ev;
  struct guest_event *gev;

  int64_t version = IVSHMEM_PROTOCOL_VERSION;
  uint64_t hostid = HOST_PEERID;

  /* Init to 0 to prevent invalid argument errors from epoll ctl */
  memset(&ev, 0, sizeof(ev));

  /* Accept connection from guest */
  cfd = accept(ctx->guest_uxfd, NULL, NULL);
  if (cfd < 0)
  {
    LOG_ERROR("accept failed");
    perror("");
    return -1;
  }

  /* Create shared memory region */
  snprintf(shm_name, sizeof(shm_name), "%s_%d", 
      CHAMELIO_SHM_NAME, ctx->guest_id_next);
  shm_base = shm_create_huge(shm_name, ctx->config->shm_len, NULL, &sfd);
  if (shm_base == NULL)
  {
    LOG_ERROR("failed to initialise shared memory for guest %d", ctx->guest_id_next);
    goto close_cfd;
  }

  /* Send protocol version as required by QEMU IVSHMEM */
  ret = uxsocket_send_int(cfd, version);
  if (ret < 0)
  {
    LOG_ERROR("failed to send protocol version");
    goto shm_destroy;
  }

  /* Send guest ID to QEMU */
  ret = uxsocket_send_int(cfd, ctx->guest_id_next);
  if (ret < 0)
  {
    LOG_ERROR("failed to send vm id");
    goto shm_destroy;
  }

  /* Send shared memory fd to qemu */
  ret = uxsocket_sendfd(cfd, sfd, -1);
  if (ret < 0)
  {
    LOG_ERROR("failed to sned shm fd");
    goto shm_destroy;
  }

  /* Create fd so that vm can interrupt host. IVSHMEM protocol needs this
     but we don't use this feature at the moment */
  if ((nfd = eventfd(0, EFD_SEMAPHORE | EFD_NONBLOCK)) < 0)
  {
    LOG_ERROR("failed to create notify fd");
    goto shm_destroy;
  }

  /* Ivshmem protocol requires to send host id with the notify fd */
  ret = uxsocket_sendfd(cfd, nfd, hostid);
  if (ret < 0)
  {
    LOG_ERROR("failed to send notify fd");
    goto close_nfd;
  }

  /* Create and send eventfd so that host can interrupt vm. 
     IVSHMEM protocol needs this but we don't use this feature 
     at the moment */
  if ((ifd = eventfd(0, EFD_SEMAPHORE | EFD_NONBLOCK)) < 0)
  {
    LOG_ERROR("failed to create interrupt fd");
    goto close_nfd;
  }

  ret = uxsocket_sendfd(cfd, ifd, ctx->guest_id_next);
  if (ret < 0)
  {
    LOG_ERROR("failed to senf interrupt fd");
    goto close_ifd;
  }

  /* Allocate guest event */
  gev = malloc(sizeof(struct guest_event));
  if (gev == NULL)
  {
    LOG_ERROR("failed to allocate guest event struct");
    goto close_ifd;
  }

  /* Add connection to epoll */
  gev->type = EP_GUEST;
  gev->fd = cfd;
  gev->gid = ctx->guest_id_next;

  ev.events = EPOLLIN | EPOLLRDHUP | EPOLLERR;
  ev.data.ptr = gev;
  ret = epoll_ctl(ctx->guest_epfd, EPOLL_CTL_ADD, cfd, &ev);
  if (ret != 0)
  {
    LOG_ERROR("epoll_ctl failed");
    perror("");
    goto free_gev;
  }

  /* TODO: Create queue for messages between guest and chamelio */


  ctx->guest_id_next++;
  return 0;

free_gev:
  free(gev);
close_ifd:
  close(ifd);
close_nfd:
  close(nfd);
shm_destroy:
  shm_destroy_huge(shm_name, ctx->config->shm_len, shm_base, sfd);
close_cfd:
  close(cfd);

  return -1;
} 

static void uxsocket_error(struct slow_context *ctx, struct guest_event *gev)
{
  LOG_WARN("removing cfd=%d from guest epfd", ctx->guest_epfd);
  epoll_ctl(ctx->guest_epfd, EPOLL_CTL_DEL, gev->fd, NULL);
  close(gev->fd);
}

static int uxsocket_send_int(int fd, int64_t i)
{
  int n;

  struct iovec iov = {
      .iov_base = &i,
      .iov_len = sizeof(i),
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

  if ((n = sendmsg(fd, &msg, 0)) != sizeof(int64_t))
  {
      LOG_ERROR("failed to send msg");
      return -1;
  }

  return n;
}

static int uxsocket_sendfd(int uxfd, int fd, int64_t i)
{
  int n;
  struct cmsghdr *chdr;

  /* Need to pass at least one byte of data to send control data */
  struct iovec iov = {
      .iov_base = &i,
      .iov_len = sizeof(i),
  };

  /* Allocate a char array but use a union to ensure that it
      is alligned properly */
  union
  {
      char buf[CMSG_SPACE(sizeof(fd))];
      struct cmsghdr align;
  } cmsg;
  memset(&cmsg, 0, sizeof(cmsg));

  /* Add control data (file descriptor) to msg */
  struct msghdr msg = {
      .msg_name = NULL,
      .msg_namelen = 0,
      .msg_iov = &iov,
      .msg_iovlen = 1,
      .msg_control = &cmsg,
      .msg_controllen = sizeof(cmsg),
      .msg_flags = 0,
  };

  /* Set message header to describe ancillary data */
  chdr = CMSG_FIRSTHDR(&msg);
  chdr->cmsg_level = SOL_SOCKET;
  chdr->cmsg_type = SCM_RIGHTS;
  chdr->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(chdr), &fd, sizeof(fd));

  if ((n = sendmsg(uxfd, &msg, 0)) != sizeof(i))
  {
    LOG_ERROR("failed to send message");
    return -1;
  }

  return n;
}
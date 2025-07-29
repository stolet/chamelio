#include <stdlib.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/un.h>
#include <sys/eventfd.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>

#include "shm.h"
#include "guestif.h"
#include "slow.h"
#include "log.h"
#include "shmalloc.h"
#include "queue.h"
#include "uxsocket.h"

#define IVSHMEM_PROTOCOL_VERSION 0
#define HOST_PEERID 255

#define EP_LISTEN_GUEST 1
#define EP_GUEST 2

static int uxsocket_init(struct slow_context *ctx);
static int uxsocket_init_fd(struct slow_context *ctx);
static int uxsocket_accept(struct slow_context *ctx);
static void uxsocket_error(struct slow_context *ctx, struct guest_event *gev);

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
        if ((evs[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) != 0)
        {
          uxsocket_error(ctx, gev);
        }
        else if ((evs[i].events & EPOLLIN) != 0)
        {
          LOG_DEBUG("EPOLLIN");
        }
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
  struct guest_slow *g;
  struct shm_allocator *alloc;
  struct shm_handle *agt_cham_handle, *cham_agent_handle;
  struct dqueue *agt_cham_q; 
  struct equeue *cham_agt_q;

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

  g = malloc(sizeof(struct guest_slow));
  if (g == NULL)
  {
    LOG_ERROR("failed to allocate guest_slow struct");
    goto free_gev;
  }
  g->id = ctx->guest_id_next;
  g->shm_fd = sfd;
  g->shm_base = shm_base;

  alloc = shmalloc_init(sfd, shm_base, ctx->config->shm_len);
  if (alloc == NULL)
  {
    LOG_ERROR("failed to initialise shm allocator");
    goto free_guest;
  }
  g->alloc = alloc;

  /* Create queue that holds messages from guest agent to Chamelio */
  ret = shmalloc_alloc(alloc, ctx->config->agt_queue_len, &agt_cham_handle);
  if (ret != 0)
  {
    LOG_ERROR("failed to allocated memory in shared memory");
    goto free_alloc;
  }

  agt_cham_q = dqueue_new(ctx->config->agt_queue_len, agt_cham_handle);
  if (agt_cham_q == NULL)
  {
    LOG_ERROR("failed to create guest->chamelio queue");
    goto free_agt_cham_handle;
  }
  assert(agt_cham_q->entries == alloc->shm_base);
  g->agt_cham_q = agt_cham_q;

  /* Create queue that holds messages from Chamelio to guest agent */
  ret = shmalloc_alloc(alloc, ctx->config->agt_queue_len, &cham_agent_handle);
  if (ret != 0)
  {
    LOG_ERROR("failed to allocated memory in shared memory");
    goto free_agt_cham_q;
  }

  cham_agt_q = equeue_new(ctx->config->agt_queue_len, cham_agent_handle);
  if (cham_agt_q == NULL)
  {
    LOG_ERROR("failed to create chamelio->guest queue");
    goto free_cham_agt_handle;
  }
  assert(cham_agt_q->entries == 
      (alloc->shm_base + ctx->config->app_queue_len));
  g->cham_agt_q = cham_agt_q;

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
    goto free_cham_agt_q;
  }  

  ctx->guest_id_next++;
  g->next = ctx->guests;
  ctx->guests = g;
  return 0;

free_cham_agt_q:
  free(cham_agt_q);
free_cham_agt_handle:
  free(cham_agent_handle);
free_agt_cham_q:
  free(agt_cham_q);
free_agt_cham_handle:
  shmalloc_free(alloc, agt_cham_handle);
free_alloc:
  free(alloc);
free_guest:
  free(g);
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
  free(gev);
}
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/un.h>
#include <sys/eventfd.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>

#include "shm.h"
#include "ivshmemif.h"
#include "slow.h"
#include "log.h"
#include "shmalloc.h"
#include "queue.h"
#include "uxsocket.h"

#define IVSHMEM_PROTOCOL_VERSION 0
#define HOST_PEERID 255

#define EP_LISTEN_VM 1
#define EP_VM 2

static int uxsocket_init(struct slow_context *ctx);
static int uxsocket_init_fd(struct slow_context *ctx);
static int uxsocket_accept(struct slow_context *ctx);
static void uxsocket_error(struct slow_context *ctx, struct ivshmem_event *gev);

int ivshmemif_init(struct slow_context *ctx)
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

int ivshmemif_poll(struct slow_context *ctx)
{
  int n, i;
  struct epoll_event evs[32];
  struct ivshmem_event *gev;

  n = epoll_wait(ctx->ivshmem_epfd, evs, 32, 0);

  for (i = 0; i < n; i++)
  {
    gev = evs[i].data.ptr;
    switch (gev->type)
    {
      case EP_LISTEN_VM:
        uxsocket_accept(ctx);
        break;
      case EP_VM:
        if ((evs[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) != 0)
        {
          uxsocket_error(ctx, gev);
        }
        break;
      default:
        LOG_WARN("unknown ivshmem event type");
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
  ctx->ivshmem_epfd = epfd;

  ret = uxsocket_init_fd(ctx);
  if (ret != 0)
  {
    LOG_ERROR("failed to init unix socket for vms");
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
  struct ivshmem_event *vmev;

  fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd == -1) 
  {
    LOG_ERROR("socket creation failed");
    perror("");
    return -1;
  }

  memset(&saun, 0, sizeof(saun));
  saun.sun_family = AF_UNIX;
  memcpy(saun.sun_path, IVSHMEM_SOCKET_PATH, sizeof(IVSHMEM_SOCKET_PATH));
  unlink(saun.sun_path);

  ret = bind(fd, (struct sockaddr *) &saun, sizeof(saun));
  if (ret != 0) 
  {
    LOG_ERROR("bind failed");
    perror("");
    goto close_fd;
  }

  ret = listen(fd, 5);
  if (ret != 0) 
  {
    LOG_ERROR("listen failed");
    perror("");
    goto close_fd;
  }

  vmev = malloc(sizeof(struct ivshmem_event));
  if (vmev == NULL)
  {
    LOG_ERROR("failed to malloc ivshmem event");
    perror("");
    goto close_fd;
  }

  vmev->type = EP_LISTEN_VM;

  ev.events = EPOLLIN;
  ev.data.ptr = vmev;
  ret = epoll_ctl(ctx->ivshmem_epfd, EPOLL_CTL_ADD, fd, &ev);
  if (ret != 0) {
    LOG_ERROR("epoll_ctl listen failed");
    perror("");
    goto free_gev;
  }

  ctx->ivshmem_uxfd = fd;

  return 0;

free_gev:
  free(vmev);
close_fd:
  close(fd);

  return -1;
}

static int uxsocket_accept(struct slow_context *ctx)
{
  int i, ret, cfd, ifd, nfd, sfd;
  void *shm_base;
  char shm_name[30];
  struct epoll_event ev;
  struct ivshmem_event *gev;
  struct guest_slow *g;
  struct shm_allocator *alloc;
  struct shm_handle *agt_cham_handle, *cham_agent_handle;
  struct dqueue *agt_cham_q; 
  struct equeue *cham_agt_q;
  struct queue_entry *qe_new_vm;
  struct queue_new_guest_req *new_guest_req;

  int64_t version = IVSHMEM_PROTOCOL_VERSION;
  uint64_t hostid = HOST_PEERID;

  /* Init to 0 to prevent invalid argument errors from epoll ctl */
  memset(&ev, 0, sizeof(ev));

  /* Accept connection from VM */
  cfd = accept(ctx->ivshmem_uxfd, NULL, NULL);
  if (cfd < 0)
  {
    LOG_ERROR("accept failed");
    perror("");
    return -1;
  }

  /* Create shared memory region */
  snprintf(shm_name, sizeof(shm_name), "%s_%d", 
      CHAMELIO_SHM_NAME, ctx->n_guests);
  shm_base = shm_create_huge(shm_name, ctx->config->shm_len, NULL, &sfd);
  if (shm_base == NULL)
  {
    LOG_ERROR("failed to initialise shared memory for guest %d", ctx->n_guests);
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
  ret = uxsocket_send_int(cfd, ctx->n_guests);
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

  ret = uxsocket_sendfd(cfd, ifd, ctx->n_guests);
  if (ret < 0)
  {
    LOG_ERROR("failed to senf interrupt fd");
    goto close_ifd;
  }

  /* Allocate guest event */
  gev = malloc(sizeof(struct ivshmem_event));
  if (gev == NULL)
  {
    LOG_ERROR("failed to allocate guest event struct");
    goto close_ifd;
  }

  g = &ctx->guests[ctx->n_guests];
  g->id = ctx->n_guests;
  g->shm_fd = sfd;
  g->shm_base = shm_base;

  alloc = shmalloc_init(sfd, shm_base, ctx->config->shm_len);
  if (alloc == NULL)
  {
    LOG_ERROR("failed to initialise shm allocator");
    goto free_gev;
  }
  g->alloc = alloc;

  /* Create queue that holds messages from guest agent to Chamelio */
  ret = shmalloc_alloc(alloc, ctx->config->agt_queue_len, &agt_cham_handle);
  if (ret != 0)
  {
    LOG_ERROR("failed to allocated memory in shared memory");
    goto free_alloc;
  }
  memset(agt_cham_handle->addr, 0, ctx->config->agt_queue_len);

  agt_cham_q = dqueue_new(ctx->config->agt_queue_len, 
      agt_cham_handle->addr, agt_cham_handle->off);
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
  memset(cham_agent_handle->addr, 0, ctx->config->agt_queue_len);

  cham_agt_q = equeue_new(ctx->config->agt_queue_len, 
      cham_agent_handle->addr, cham_agent_handle->off);
  if (cham_agt_q == NULL)
  {
    LOG_ERROR("failed to create chamelio->guest queue");
    goto free_cham_agt_handle;
  }
  assert(cham_agt_q->entries == 
      (alloc->shm_base + ctx->config->app_queue_len));
  g->cham_agt_q = cham_agt_q;

  /* Add connection to epoll */
  gev->type = EP_VM;
  gev->fd = cfd;
  gev->vmid = ctx->n_guests;

  ev.events = EPOLLIN | EPOLLRDHUP | EPOLLERR;
  ev.data.ptr = gev;
  ret = epoll_ctl(ctx->ivshmem_epfd, EPOLL_CTL_ADD, cfd, &ev);
  if (ret != 0)
  {
    LOG_ERROR("epoll_ctl failed");
    perror("");
    goto free_cham_agt_q;
  }

  /* Register new guest with the fast-path */
  for (i = 0; i < ctx->config->fp_cores_max; i++)
  {
    qe_new_vm = queue_tail(ctx->slow_fast_qs[i]);
    if (qe_new_vm == NULL)
    {
      LOG_ERROR("slow to fast queue is empty");
      goto remove_from_epoll;
    }

    new_guest_req = (struct queue_new_guest_req *) &qe_new_vm->data;
    new_guest_req->id = g->id;
    new_guest_req->shm_base = shm_base;
    new_guest_req->shm_len = ctx->config->shm_len;
    ret = queue_enqueue(ctx->slow_fast_qs[i], QUEUE_NEW_GUEST);
    if (ret != 0)
    {
      LOG_ERROR("failed to enqueue new guest req to fast-path");
      goto remove_from_epoll;
    }
  }

  ctx->n_guests++;
  return 0;

remove_from_epoll:
  epoll_ctl(ctx->ivshmem_epfd, EPOLL_CTL_DEL, gev->fd, NULL);
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

static void uxsocket_error(struct slow_context *ctx, struct ivshmem_event *gev)
{
  LOG_WARN("removing cfd=%d from guest epfd", ctx->ivshmem_epfd);
  epoll_ctl(ctx->ivshmem_epfd, EPOLL_CTL_DEL, gev->fd, NULL);
  close(gev->fd);
  free(gev);
}
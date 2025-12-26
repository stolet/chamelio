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
#include "control.h"
#include "log.h"
#include "shmalloc.h"
#include "queue_fns.h"
#include "queue_types.h"
#include "uxsocket.h"

#define IVSHMEM_PROTOCOL_VERSION 0
#define HOST_PEERID 255

#define EP_LISTEN_VM 1
#define EP_VM 2

static int uxsocket_init(struct control_context *ctx);
static int uxsocket_init_fd(struct control_context *ctx);
static int uxsocket_accept(struct control_context *ctx);
static void uxsocket_error(struct control_context *ctx, struct ivshmem_event *gev);

int ivshmemif_init(struct control_context *ctx)
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

int ivshmemif_poll(struct control_context *ctx)
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

static int uxsocket_init(struct control_context *ctx)
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

static int uxsocket_init_fd(struct control_context *ctx)
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

static int uxsocket_accept(struct control_context *ctx)
{
  int i, ret, cfd, sfd, nfd, ifd;
  void *shm_base;
  char shm_name[30];
  struct epoll_event ev;
  struct ivshmem_event *iev;
  struct guest_control *g;
  struct shm_allocator *alloc;
  struct shm_handle *guest_cham_handle, *cham_guest_handle;
  struct dqueue *guest_cham_q;
  struct equeue *cham_guest_q;
  struct queue_entry *qe_new_guest;
  struct queue_new_guest_req *new_guest_req;

  int64_t version = IVSHMEM_PROTOCOL_VERSION;
  __u64 hostid = HOST_PEERID;

  /* Init to 0 to prevent invalid argument errors from epoll ctl */
  memset(&ev, 0, sizeof(ev));

  /* Accept connection from guest */
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
    LOG_ERROR("failed to send interrupt fd");
    goto close_ifd;
  }

  /* Allocate guest event */
  iev = malloc(sizeof(struct ivshmem_event));
  if (iev == NULL)
  {
    LOG_ERROR("failed to allocate app event struct");
    goto close_ifd;
  }

  /* Allocate control path struct for guest */
  g = &ctx->guests[ctx->n_guests];
  g->id = ctx->n_guests;
  g->budget = ctx->config->perf_iso_cap;
  g->shm_fd = sfd;
  g->shm_base = shm_base;

  alloc = shmalloc_init(sfd, shm_base, ctx->config->shm_len);
  if (alloc == NULL)
  {
    LOG_ERROR("failed to initialise shm allocator");
    goto free_iev;
  }
  g->alloc = alloc;

  /* Create queue that holds messages from guest to Chamelio */
  ret = shmalloc_alloc(alloc, ctx->config->agt_queue_len * 
      sizeof(struct queue_entry), &guest_cham_handle);
  if (ret != 0)
  {
    LOG_ERROR("failed to allocate memory in shared memory");
    goto free_alloc;
  }
  memset(guest_cham_handle->addr, 0, ctx->config->agt_queue_len);

  guest_cham_q = dqueue_new(ctx->config->agt_queue_len, 
      sizeof(struct queue_entry),
      guest_cham_handle->addr, guest_cham_handle->off);
  if (guest_cham_q == NULL)
  {
    LOG_ERROR("failed to create guest->chamelio queue");
    goto free_guest_cham_handle;
  }
  assert(guest_cham_q->off == 0);
  g->guest_cham_q = guest_cham_q;

  /* Create queue that holds messages from Chamelio to guest */
  ret = shmalloc_alloc(alloc, ctx->config->agt_queue_len * 
    sizeof(struct queue_entry), &cham_guest_handle);
  if (ret != 0)
  {
    LOG_ERROR("failed to allocated memory in shared memory");
    goto free_guest_cham_q;
  }
  memset(cham_guest_handle->addr, 0, ctx->config->agt_queue_len);

  cham_guest_q = equeue_new(ctx->config->agt_queue_len, 
      sizeof(struct queue_entry),
      cham_guest_handle->addr, cham_guest_handle->off);
  if (cham_guest_q == NULL)
  {
    LOG_ERROR("failed to create chamelio->guest queue");
    goto free_cham_guest_handle;
  }
  assert(cham_guest_q->off == 
      ctx->config->agt_queue_len * sizeof(struct queue_entry));
  g->cham_guest_q = cham_guest_q;

  /* Add connection to epoll */
  iev->type = EP_VM;
  iev->fd = cfd;
  iev->guest = g;

  ev.events = EPOLLIN | EPOLLRDHUP | EPOLLERR;
  ev.data.ptr = iev;
  ret = epoll_ctl(ctx->ivshmem_epfd, EPOLL_CTL_ADD, cfd, &ev);
  if (ret != 0)
  {
    LOG_ERROR("epoll_ctl failed");
    perror("");
    goto free_cham_guest_q;
  }  

  /* Register new guest with the fast-path cores */
  for (i = 0; i < ctx->config->fp_cores_max; i++)
  {
    qe_new_guest = queue_tail(ctx->ctl_fast_qs[i]);
    if (qe_new_guest == NULL)
    {
      LOG_ERROR("control to fast queue is empty");
      goto free_cham_guest_q;
    }

    new_guest_req = &qe_new_guest->data.new_guest_req;
    new_guest_req->id = g->id;
    new_guest_req->budget = &g->budget;
    new_guest_req->shm_base = shm_base;
    new_guest_req->shm_len = ctx->config->shm_len;
    ret = queue_enqueue(ctx->ctl_fast_qs[i], QUEUE_NEW_GUEST_REQ);
    if (ret != 0)
    {
      LOG_ERROR("failed to enqueue new guest req to fast-path");
      goto free_cham_guest_q;
    }
  }

  ctx->n_guests++;

  LOG_DEBUG("registered new vm=%d", g->id);
  return 0;

free_cham_guest_q:
  free(cham_guest_q);
free_cham_guest_handle:
  shmalloc_free(alloc, cham_guest_handle);
free_guest_cham_q:
  free(guest_cham_q);
free_guest_cham_handle:
  shmalloc_free(alloc, guest_cham_handle);
free_alloc:
  free(alloc);
free_iev:
  free(iev);

  
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

static void uxsocket_error(struct control_context *ctx, struct ivshmem_event *gev)
{
  LOG_WARN("removing cfd=%d from guest epfd", ctx->ivshmem_epfd);
  epoll_ctl(ctx->ivshmem_epfd, EPOLL_CTL_DEL, gev->fd, NULL);
  close(gev->fd);
  free(gev);
}
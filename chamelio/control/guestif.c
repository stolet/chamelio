#include <stdlib.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/un.h>
#include <sys/eventfd.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>

#include "ivshmemif.h"
#include "guestif.h"
#include "shm.h"
#include "control.h"
#include "log.h"
#include "shmalloc.h"
#include "queue.h"
#include "uxsocket.h"

#define EP_LISTEN_GUEST 1
#define EP_GUEST 2

static int uxsocket_init(struct control_context *ctx);
static int uxsocket_init_fd(struct control_context *ctx);
static int uxsocket_accept(struct control_context *ctx);
static void uxsocket_error(struct control_context *ctx, struct guest_event *gev);
static void uxsocket_receive(struct control_context *ctx, struct guest_event *gev);

int guestif_init(struct control_context *ctx)
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

int guestif_poll(struct control_context *ctx)
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
          uxsocket_receive(ctx, gev);
        }
        break;
      default:
        LOG_WARN("unknown guest event type");
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
  ctx->guest_epfd = epfd;

  ret = uxsocket_init_fd(ctx);
  if (ret != 0)
  {
    LOG_ERROR("failed to init unix socket for apps");
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
    LOG_ERROR("failed to malloc app event");
    perror("");
    goto error_close;
  }
  gev->type = EP_LISTEN_GUEST;
  gev->guest = NULL;
  gev->fd = -1;
  gev->req_rx = 0;
  gev->resp_sz = 0;

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

static int uxsocket_accept(struct control_context *ctx)
{
  int i, ret, cfd, sfd;
  void *shm_base;
  char shm_name[30];
  struct epoll_event ev;
  struct guest_event *gev;
  struct guest_control *g;
  struct shm_allocator *alloc;
  struct shm_handle *guest_cham_handle, *cham_guest_handle;
  struct dqueue *guest_cham_q;
  struct equeue *cham_guest_q;
  struct queue_entry *qe_new_guest;
  struct queue_new_guest_req *new_guest_req;

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
      CHAMELIO_SHM_NAME, ctx->n_guests);
  shm_base = shm_create_huge(shm_name, ctx->config->shm_len, NULL, &sfd);
  if (shm_base == NULL)
  {
    LOG_ERROR("failed to initialise shared memory for guest %d", ctx->n_guests);
    goto close_cfd;
  }

  /* Send shared memory fd to application */
  ret = uxsocket_sendfd(cfd, sfd, -1);
  if (ret < 0)
  {
    LOG_ERROR("failed to sned shm fd");
    goto shm_destroy;
  }

  /* Allocate guest event */
  gev = malloc(sizeof(struct guest_event));
  if (gev == NULL)
  {
    LOG_ERROR("failed to allocate app event struct");
    goto shm_destroy;
  }

  /* Allocate control path struct for guest */
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
  gev->type = EP_GUEST;
  gev->fd = cfd;
  gev->req_rx = 0;
  gev->resp_sz = 0;
  gev->guest = g;

  ev.events = EPOLLIN | EPOLLRDHUP | EPOLLERR;
  ev.data.ptr = gev;
  ret = epoll_ctl(ctx->guest_epfd, EPOLL_CTL_ADD, cfd, &ev);
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
free_gev:
  free(gev);
shm_destroy:
  shm_destroy_huge(shm_name, ctx->config->shm_len, shm_base, sfd);
close_cfd:
  close(cfd);

  return -1;
} 

static void uxsocket_error(struct control_context *ctx, struct guest_event *gev)
{
  LOG_WARN("removing cfd=%d from guest epfd", ctx->guest_epfd);
  epoll_ctl(ctx->guest_epfd, EPOLL_CTL_DEL, gev->fd, NULL);
  close(gev->fd);
  free(gev);
}

static void uxsocket_receive(struct control_context *ctx, struct guest_event *gev)
{
  int n;
  size_t res_sz;
  struct proto_control *p;

  struct iovec iov = {
    .iov_base = &gev->proto_req,
    .iov_len = sizeof(gev->proto_req) - gev->req_rx,
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

  n = recvmsg(gev->fd, &msg, 0);
  if (n < 0)
  {
    LOG_ERROR("failed to recvmsg");
    goto error_uxsocket;
  }

  if(msg.msg_controllen > 0) 
  {
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    assert(cmsg->cmsg_len == CMSG_LEN(sizeof(int)));
  }

  if (n < 0)
  {
    LOG_ERROR("recv failed");
    perror("");
    goto error_uxsocket;
  }
  else if (n + gev->req_rx < sizeof(gev->proto_req))
  {
    /* Request not complete yet */
    gev->req_rx += n;
    return;
  }

  /* Request complete */
  gev->req_rx = 0;
  p = &gev->guest->proto;
  p->guest = gev->guest;
  p->nqueues = 0;
  p->nmaps = 0;

  /* Initialise response */
  gev->proto_res.n_fp_cores = ctx->config->fp_cores_max;
  gev->proto_res.shm_len = ctx->config->shm_len;
  gev->proto_res.guestq_nelems = ctx->config->agt_queue_len;
  gev->proto_res.guestq_elsize = sizeof(struct queue_entry);

  /* Send out response */
  res_sz = sizeof(struct queue_new_proto_res);
  n = send(gev->fd, &gev->proto_res, res_sz, 0);
  if (n < 0) 
  {
    LOG_ERROR("send failed");
    perror("");
    goto error_uxsocket;
  } 
  else if (n < res_sz) 
  {
    LOG_ERROR("short send for response");
    goto error_uxsocket;
  }

  return;

error_uxsocket:
    uxsocket_error(ctx, gev);
}

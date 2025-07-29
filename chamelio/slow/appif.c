#include <stdlib.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/un.h>
#include <sys/eventfd.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>

#include "guestif.h"
#include "appif.h"
#include "shm.h"
#include "slow.h"
#include "log.h"
#include "shmalloc.h"
#include "queue.h"
#include "uxsocket.h"

#define EP_LISTEN_APP 1
#define EP_APP 2

static int uxsocket_init(struct slow_context *ctx);
static int uxsocket_init_fd(struct slow_context *ctx);
static int uxsocket_accept(struct slow_context *ctx);
static void uxsocket_error(struct slow_context *ctx, struct app_event *aev);
static void uxsocket_receive(struct slow_context *ctx, struct app_event *aev);

int appif_init(struct slow_context *ctx)
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

int appif_poll(struct slow_context *ctx)
{
  int n, i;
  struct epoll_event evs[32];
  struct app_event *aev;

  n = epoll_wait(ctx->app_epfd, evs, 32, 0);

  for (i = 0; i < n; i++)
  {
    aev = evs[i].data.ptr;
    switch (aev->type)
    {
      case EP_LISTEN_APP:
        uxsocket_accept(ctx);
        break;
      case EP_APP:
        if ((evs[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) != 0)
        {
          uxsocket_error(ctx, aev);
        }
        else if ((evs[i].events & EPOLLIN) != 0)
        {
          uxsocket_receive(ctx, aev);
          LOG_DEBUG("EPOLLIN");
        }
        break;
      default:
        LOG_WARN("unknown app event type");
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
  ctx->app_epfd = epfd;

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

static int uxsocket_init_fd(struct slow_context *ctx)
{
  int fd, ret;
  struct epoll_event ev;
  struct sockaddr_un saun;
  struct app_event *aev;

  fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd == -1) 
  {
    LOG_ERROR("socket creation failed");
    perror("");
    return -1;
  }

  memset(&saun, 0, sizeof(saun));
  saun.sun_family = AF_UNIX;
  memcpy(saun.sun_path, APP_SOCKET_PATH, sizeof(APP_SOCKET_PATH));
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

  aev = malloc(sizeof(struct app_event));
  if (aev == NULL)
  {
    LOG_ERROR("failed to malloc app event");
    perror("");
    goto error_close;
  }
  aev->type = EP_LISTEN_APP;
  aev->app = NULL;
  aev->guest = NULL;
  aev->fd = -1;
  aev->req_rx = 0;
  aev->resp_sz = 0;

  ev.events = EPOLLIN;
  ev.data.ptr = aev;
  ret = epoll_ctl(ctx->app_epfd, EPOLL_CTL_ADD, fd, &ev);
  if (ret != 0) {
    LOG_ERROR("epoll_ctl listen failed");
    perror("");
    goto error_free_aev;
  }

  ctx->app_uxfd = fd;

  return 0;

error_free_aev:
  free(aev);
error_close:
  close(fd);

  return -1;
}

static int uxsocket_accept(struct slow_context *ctx)
{
  int ret, cfd, sfd;
  void *shm_base;
  char shm_name[30];
  struct epoll_event ev;
  struct app_event *aev;
  struct app_slow *a;
  struct guest_slow *g;
  struct shm_allocator *alloc;
  struct shm_handle *agt_cham_handle, *cham_agt_handle;
  struct dqueue *agt_cham_q;
  struct equeue *cham_agt_q;

  /* Init to 0 to prevent invalid argument errors from epoll ctl */
  memset(&ev, 0, sizeof(ev));

  /* Accept connection from app */
  cfd = accept(ctx->app_uxfd, NULL, NULL);
  if (cfd < 0)
  {
    LOG_ERROR("accept failed");
    perror("");
    return -1;
  }

  /* Create shared memory region */
  snprintf(shm_name, sizeof(shm_name), "%s_%d", 
      CHAMELIO_SHM_NAME, ctx->app_id_next);
  shm_base = shm_create_huge(shm_name, ctx->config->shm_len, NULL, &sfd);
  if (shm_base == NULL)
  {
    LOG_ERROR("failed to initialise shared memory for app %d", ctx->app_id_next);
    goto close_cfd;
  }

  /* Send shared memory fd to application */
  ret = uxsocket_sendfd(cfd, sfd, -1);
  if (ret < 0)
  {
    LOG_ERROR("failed to sned shm fd");
    goto shm_destroy;
  }

  /* Allocate app event */
  aev = malloc(sizeof(struct app_event));
  if (aev == NULL)
  {
    LOG_ERROR("failed to allocate app event struct");
    goto shm_destroy;
  }

  /* Allocate slow path struct for guest */
  g = malloc(sizeof(struct guest_slow));
  if (g == NULL)
  {
    LOG_ERROR("failed to allocate guest_slow struct");
    goto free_aev;
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
  memset(agt_cham_handle->addr, 0, ctx->config->agt_queue_len);

  agt_cham_q = dqueue_new(ctx->config->agt_queue_len, agt_cham_handle);
  if (agt_cham_q == NULL)
  {
    LOG_ERROR("failed to create guest->chamelio queue");
    goto free_agt_cham_handle;
  }
  assert(agt_cham_q->entries == alloc->shm_base);
  g->agt_cham_q = agt_cham_q;

  /* Create queue that holds messages from Chamelio to guest agent */
  ret = shmalloc_alloc(alloc, ctx->config->agt_queue_len, &cham_agt_handle);
  if (ret != 0)
  {
    LOG_ERROR("failed to allocated memory in shared memory");
    goto free_agt_cham_q;
  }
  memset(cham_agt_handle->addr, 0, ctx->config->agt_queue_len);

  cham_agt_q = equeue_new(ctx->config->agt_queue_len, cham_agt_handle);
  if (cham_agt_q == NULL)
  {
    LOG_ERROR("failed to create chamelio->guest queue");
    goto free_cham_agt_handle;
  }
  assert(cham_agt_q->entries == 
      (alloc->shm_base + ctx->config->app_queue_len));
  g->cham_agt_q = cham_agt_q;

  /* Allocate slow path struct for application */
  a = malloc(sizeof(struct app_slow));
  if (a == NULL)
  {
    LOG_ERROR("failed to allocate app_slow struct");
    goto free_cham_agt_q;
  }
  a->id = ctx->app_id_next;
  a->guest = g;
  a->ctxs = NULL;

  /* Add connection to epoll */
  aev->type = EP_APP;
  aev->fd = cfd;
  aev->guest = g;
  aev->app = a;

  ev.events = EPOLLIN | EPOLLRDHUP | EPOLLERR;
  ev.data.ptr = aev;
  ret = epoll_ctl(ctx->app_epfd, EPOLL_CTL_ADD, cfd, &ev);
  if (ret != 0)
  {
    LOG_ERROR("epoll_ctl failed");
    perror("");
    goto free_app;
  }  

  ctx->guest_id_next++;
  ctx->app_id_next++;
  a->next = g->apps;
  g->apps = a;
  g->next = ctx->guests;
  ctx->guests = g;
  return 0;

free_app:
  free(a);
free_cham_agt_q:
  free(cham_agt_q);
free_cham_agt_handle:
  shmalloc_free(alloc, cham_agt_handle);
free_agt_cham_q:
  free(agt_cham_q);
free_agt_cham_handle:
  shmalloc_free(alloc, agt_cham_handle);
free_alloc:
  free(alloc);
free_guest:
  free(g);
free_aev:
  free(aev);
shm_destroy:
  shm_destroy_huge(shm_name, ctx->config->shm_len, shm_base, sfd);
close_cfd:
  close(cfd);

  return -1;
} 

static void uxsocket_error(struct slow_context *ctx, struct app_event *aev)
{
  LOG_WARN("removing cfd=%d from app epfd", ctx->app_epfd);
  epoll_ctl(ctx->app_epfd, EPOLL_CTL_DEL, aev->fd, NULL);
  close(aev->fd);
  free(aev);
}

static void uxsocket_receive(struct slow_context *ctx, struct app_event *aev)
{
  int n, i, ret;
  size_t res_sz;
  struct app_context *app_ctx;
  struct dqueue *app_cham_q; 
  struct equeue *cham_app_q;
  struct shm_handle **rxq, **txq, *ac_handle, *ca_handle;

  struct shm_allocator *alloc = aev->guest->alloc;

  struct iovec iov = {
    .iov_base = &aev->ctx_req,
    .iov_len = sizeof(aev->ctx_req) - aev->req_rx,
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

  n = recvmsg(aev->fd, &msg, 0);

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
  else if (n + aev->req_rx < sizeof(aev->ctx_req))
  {
    /* Request not complete yet */
    aev->req_rx += n;
    return;
  }

  /* Request complete */
  aev->req_rx = 0;

  /* Allocate application context */
  app_ctx = malloc(sizeof(struct app_context));
  if (app_ctx == NULL)
  {
    LOG_ERROR("failed to allocated app_ctx");
    goto error_uxsocket;
  }

  /* Allocate list to hold shm handles for rx queues */
  rxq = malloc(sizeof(struct shm_handle *) * ctx->config->fp_cores_max);
  if (rxq == NULL)
  {
    LOG_ERROR("failed to allocate rxq");
    goto free_app_ctx;
  }
  app_ctx->rxq = rxq;

  /* Allocate list to hold shm handles for tx queues */
  txq = malloc(sizeof(struct shm_handle *) * ctx->config->fp_cores_max);
  if (txq == NULL)
  {
    LOG_ERROR("failed to allocate txq");
    goto free_rxq;
  }
  app_ctx->txq = txq;

  /* Allocate memory in the shared memory region for each queue */
  for (i = 0; i < ctx->config->fp_cores_max; i++)
  {
    if (shmalloc_alloc(alloc, aev->ctx_req.rxq_len, &app_ctx->rxq[i]))
    {
      LOG_ERROR("shmalloc_alloc for rxq=%d failed", i);
      goto free_txq;
    }
    
    if (shmalloc_alloc(alloc, aev->ctx_req.txq_len, &app_ctx->txq[i]))
    {
      LOG_ERROR("shmalloc_alloc for txq=%d failed", i);
      goto free_txq;
    }
    
    memset((uint8_t *) app_ctx->rxq[i]->addr, 0, aev->ctx_req.rxq_len);
    memset((uint8_t *) app_ctx->txq[i]->addr, 0, aev->ctx_req.txq_len);
    aev->ctx_res.rxq_offs[i] = app_ctx->rxq[i]->off;
    aev->ctx_res.txq_offs[i] = app_ctx->txq[i]->off;
  }

  /* Create queue that holds messages from the app context to Chamelio */
  ret = shmalloc_alloc(alloc, ctx->config->app_queue_len, &ac_handle);
  if (ret != 0)
  {
    LOG_ERROR("failed to allocated memory in shared memory");
    goto free_shm_allocs;
  }
  memset(ac_handle->addr, 0, ctx->config->app_queue_len);

  app_cham_q = dqueue_new(ctx->config->app_queue_len, ac_handle);
  if (app_cham_q == NULL)
  {
    LOG_ERROR("failed to create guest->chamelio queue");
    goto free_app_cham_handle;
  }
  app_ctx->app_cham_q = app_cham_q;

  /* Create queue that holds messages from Chamelio to the app context */
  ret = shmalloc_alloc(alloc, ctx->config->app_queue_len, &ca_handle);
  if (ret != 0)
  {
    LOG_ERROR("failed to allocated memory in shared memory");
    goto free_app_cham_q;
  }
  memset(ca_handle->addr, 0, ctx->config->app_queue_len);

  cham_app_q = equeue_new(ctx->config->app_queue_len, ca_handle);
  if (cham_app_q == NULL)
  {
    LOG_ERROR("failed to create chamelio->app queue");
    goto free_cham_app_handle;
  }
  app_ctx->cham_app_q = cham_app_q;

  /* Add context to application */
  app_ctx->app = aev->app;
  app_ctx->next = aev->app->ctxs;
  aev->app->ctxs = app_ctx;

  /* Initialise response */
  aev->ctx_res.n_fp_cores = ctx->config->fp_cores_max;
  aev->ctx_res.cham_app_q_off = cham_app_q->sh->off;
  aev->ctx_res.cham_app_q_len = cham_app_q->sh->len;
  aev->ctx_res.app_cham_q_off = app_cham_q->sh->off;
  aev->ctx_res.app_cham_q_len = app_cham_q->sh->len;
  /* TODO: Add the actual queue offsets. This is a list so
     we need a struct larger than a cache line size */
  // aev->ctx_resp->rxq_offs =;
  // aev->ctx_resp->txq_offs =;

  /* Send out response */
  res_sz = sizeof(struct queue_new_app_ctx_res);
  n = send(aev->fd, &aev->ctx_res, res_sz, 0);
  if (n < 0) 
  {
    LOG_ERROR("send failed");
    perror("");
    goto free_cham_app_q;
  } 
  else if (n < res_sz) 
  {
    LOG_ERROR("short send for response");
    goto free_cham_app_q;
  }

  return;

free_cham_app_q:
  free(cham_app_q);
free_cham_app_handle:
  shmalloc_free(alloc, ca_handle);
free_app_cham_q:
  free(app_cham_q);
free_app_cham_handle:
  shmalloc_free(alloc, ac_handle);
free_shm_allocs:
    for (i = 0; i < ctx->config->fp_cores_max; i++)
    {
      shmalloc_free(alloc, app_ctx->rxq[i]);
      shmalloc_free(alloc, app_ctx->txq[i]);
    }
free_txq:
    free(txq);
free_rxq:
    free(rxq);
free_app_ctx:
    free(app_ctx);
error_uxsocket:
    uxsocket_error(ctx, aev);
}

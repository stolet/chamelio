#include <stdlib.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/un.h>
#include <sys/eventfd.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <cham_lib.h>

#include "log.h"
#include "appif.h"
#include "udp_slow.h"
#include "udp_queue.h"
#include "uxsocket.h"

#define EP_LISTEN_APP 1
#define EP_APP 2

static int uxsocket_init(struct udp_slow_context *ctx);
static int uxsocket_init_fd(struct udp_slow_context *ctx);
static int uxsocket_accept(struct udp_slow_context *ctx);
static void uxsocket_error(struct udp_slow_context *ctx, struct app_event *aev);
static void uxsocket_receive(struct udp_slow_context *ctx, struct app_event *aev);

int appif_init(struct udp_slow_context *ctx)
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

int appif_poll(struct udp_slow_context *ctx)
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
        }
        break;
      default:
        LOG_WARN("unknown app event type");
    }
  }

  return n;
}

static int uxsocket_init(struct udp_slow_context *ctx)
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

static int uxsocket_init_fd(struct udp_slow_context *ctx)
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
  aev->fd = -1;
  aev->req_rx = 0;
  aev->resp_sz = 0;
  aev->app = NULL;

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

static int uxsocket_accept(struct udp_slow_context *ctx)
{
  int ret, cfd;
  struct epoll_event ev;
  struct udp_app_slow *a;
  struct app_event *aev;
  struct proto_map_lib *socks_map, *offs_map, *ready_map;
  struct proto_map_lib *sched_map, *app_bump_map, *fast_bump_map;
  struct udp_off_mape *offs_table;

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

  /* Send shared memory fd to application */
  ret = uxsocket_sendfd(cfd, ctx->proto->guest->shm_fd, -1);
  if (ret < 0)
  {
    LOG_ERROR("failed to sned shm fd");
    goto close_cfd;
  }

  /* Allocate app event */
  aev = malloc(sizeof(struct app_event));
  if (aev == NULL)
  {
    LOG_ERROR("failed to allocate app event struct");
    goto close_cfd;
  }

  /* Allocate struct for app */
  a = &ctx->apps[ctx->n_apps];
  a->id = ctx->n_apps;
  a->n_ctxs = 0;

  /* Create map used to hold offsets to other maps.
     This must always be the first map created
     because the fast-path uses index 0 to access it */
  offs_map = cham_new_map(ctx->proto, MAX_OFFS, 
      sizeof(struct udp_off_mape));
  if (offs_map == NULL)
  {
    LOG_ERROR("faield to create map to hold offsets");
    goto free_aev;
  }
  a->offs_map = offs_map;
  offs_table = ctx->proto->shm_base + offs_map->off;

  /* Create map used to hold sockets */
  socks_map = cham_new_map(ctx->proto, MAX_SOCKETS, 
      sizeof(struct udp_sock_mape));
  if (socks_map == NULL)
  {
    LOG_ERROR("failed to create map to hold sockets");
    goto free_aev;
  }

  /* Create map used to hold scheduler data */
  sched_map = cham_new_map(ctx->proto, MAX_SCHED, 
      sizeof(struct udp_txsched_mape));
  if (sched_map == NULL)
  {
    LOG_ERROR("failed to create map to hold tx sched");
    goto free_aev;
  }

  /* Create map used to hold data ready to transmit */
  ready_map = cham_new_map(ctx->proto, MAX_READY, 
      sizeof(struct udp_txready_mape));
  if (ready_map == NULL)
  {
    LOG_ERROR("failed to create map to hold tx ready");
    goto free_aev;
  }
  
  /* Create map used to hold list of queues to bump app */
  app_bump_map = cham_new_map(ctx->proto, MAX_PROTO_QUEUES, 
      sizeof(struct udp_app_bump_mape));
  if (app_bump_map == NULL)
  {
    LOG_ERROR("failed to create map to hold app bump queues");
    goto free_aev;
  }

  /* Create map used to hold list of queues to bump fast-path */
  fast_bump_map = cham_new_map(ctx->proto, MAX_PROTO_QUEUES, 
      sizeof(struct udp_fast_bump_mape));
  if (fast_bump_map == NULL)
  {
    LOG_ERROR("failed to create map to hold fast-path bump queues");
    goto free_aev;
  }

  /* Add sockets map to offset table */
  offs_table[MTYPE_SOCKS].head = ID_INVALID;
  offs_table[MTYPE_SOCKS].tail = ID_INVALID;
  offs_table[MTYPE_SOCKS].id = MTYPE_SOCKS;
  offs_table[MTYPE_SOCKS].off = socks_map->off;
  offs_table[MTYPE_SOCKS].n = 0;
  offs_table[MTYPE_SOCKS].max_n = socks_map->nelems;

  /* Add scheduler map to offset table */
  offs_table[MTYPE_TXSCHED].head = ID_INVALID;
  offs_table[MTYPE_TXSCHED].tail = ID_INVALID;
  offs_table[MTYPE_TXSCHED].id = MTYPE_SOCKS;
  offs_table[MTYPE_TXSCHED].off = sched_map->off;
  offs_table[MTYPE_TXSCHED].n = 0;
  offs_table[MTYPE_TXSCHED].max_n = sched_map->nelems;
  
  /* Add tx ready map to offset table */
  offs_table[MTYPE_TXREADY].head = ID_INVALID;
  offs_table[MTYPE_TXREADY].tail = ID_INVALID;
  offs_table[MTYPE_TXREADY].id = MTYPE_TXREADY;
  offs_table[MTYPE_TXREADY].off = ready_map->off;
  offs_table[MTYPE_TXREADY].n = 0;
  offs_table[MTYPE_TXREADY].max_n = ready_map->nelems;
  
  /* Add app bump queue map to offset table */
  offs_table[MTYPE_APP_BUMPQ].head = ID_INVALID;
  offs_table[MTYPE_APP_BUMPQ].tail = ID_INVALID;
  offs_table[MTYPE_APP_BUMPQ].id = MTYPE_APP_BUMPQ;
  offs_table[MTYPE_APP_BUMPQ].off = app_bump_map->off;
  offs_table[MTYPE_APP_BUMPQ].n = 0;
  offs_table[MTYPE_APP_BUMPQ].max_n = app_bump_map->nelems;

  /* Add fast-path bump queue map to offset table */
  offs_table[MTYPE_FAST_BUMPQ].head = ID_INVALID;
  offs_table[MTYPE_FAST_BUMPQ].tail = ID_INVALID;
  offs_table[MTYPE_FAST_BUMPQ].id = MTYPE_FAST_BUMPQ;
  offs_table[MTYPE_FAST_BUMPQ].off = fast_bump_map->off;
  offs_table[MTYPE_FAST_BUMPQ].n = 0;
  offs_table[MTYPE_FAST_BUMPQ].max_n = fast_bump_map->nelems;

  /* Add connection to epoll */
  aev->type = EP_APP;
  aev->fd = cfd;
  aev->req_rx = 0;
  aev->resp_sz = 0;
  aev->app = a;

  ev.events = EPOLLIN | EPOLLRDHUP | EPOLLERR;
  ev.data.ptr = aev;
  ret = epoll_ctl(ctx->app_epfd, EPOLL_CTL_ADD, cfd, &ev);
  if (ret != 0)
  {
    LOG_ERROR("epoll_ctl failed");
    perror("");
    goto free_aev;
  }  

  ctx->n_apps++;

  return 0;

free_aev:
  free(aev);
close_cfd:
  close(cfd);
  return -1;
} 

static void uxsocket_error(struct udp_slow_context *ctx, struct app_event *aev)
{
  LOG_WARN("removing cfd=%d from app epfd", ctx->app_epfd);
  epoll_ctl(ctx->app_epfd, EPOLL_CTL_DEL, aev->fd, NULL);
  close(aev->fd);
  free(aev);
}

static void uxsocket_receive(struct udp_slow_context *ctx, struct app_event *aev)
{
  int i, n;
  size_t res_sz;
  struct equeue *eq;
  struct dqueue *dq;
  struct proto_queue_lib *q;
  struct udp_off_mape *offs_table;
  struct udp_app_bump_mape *app_bump_table;
  struct udp_fast_bump_mape *fast_bump_table;
  struct udp_app_context_slow *actx;

  struct iovec iov = {
    .iov_base = &aev->app_req,
    .iov_len = sizeof(aev->app_req) - aev->req_rx,
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
  else if (n + aev->req_rx < sizeof(aev->app_req))
  {
    /* Request not complete yet */
    aev->req_rx += n;
    return;
  }

  /* Request complete */
  aev->req_rx = 0;
  actx = &aev->app->ctxs[aev->app->n_ctxs];

  /* Create queue for messages app->slow */
  q = cham_new_queue(ctx->proto, 16384);
  if (q == NULL)
  {
    LOG_ERROR("failed to create queue app->slow");
    goto error_uxsocket;
  }

  dq = dqueue_new(q->size, 
      ctx->proto->shm_base + q->off, q->off);
  if (dq == NULL)
  {
    LOG_ERROR("failed to create dqueue for app->slow");
    goto error_uxsocket;
  }
  
  aev->app_res.as_len = q->size;
  aev->app_res.as_off = q->off;
  actx->app_slow_q = dq;
  
  /* Create queue for messages slow->app */
  q = cham_new_queue(ctx->proto, 16384);
  if (q == NULL)
  {
    LOG_ERROR("failed to create queue slow->app");
    goto error_uxsocket;
  }
  
  eq = equeue_new(q->size, 
      ctx->proto->shm_base + q->off, q->off);
  if (eq == NULL)
  {
    LOG_ERROR("failed to create equeue for slow->app");
    goto error_uxsocket;
  }
  
  aev->app_res.sa_len = q->size;
  aev->app_res.sa_off = q->off;
  actx->slow_app_q = eq;
  
  offs_table = ctx->proto->shm_base + aev->app->offs_map->off;
  fast_bump_table = ctx->proto->shm_base + offs_table[MTYPE_FAST_BUMPQ].off;
  app_bump_table = ctx->proto->shm_base + offs_table[MTYPE_APP_BUMPQ].off;
  for (i = 0; i < ctx->proto->n_fp_cores; i++)
  {
    /* Create queue for bumps fast->app */
    q = cham_new_queue(ctx->proto, 16384);
    if (q == NULL)
    {
      LOG_ERROR("failed to create queue fast->app core=%d", i);
      goto error_uxsocket;
    }
    aev->app_res.fa_len = q->size;
    aev->app_res.fa_offs[i] = q->off;
    actx->app_bump_qs[i] = q;
    
    /* We need to add these queues to the bump table so
       the fast-path TX can bump the app when it's done */
    app_bump_table[q->id].id = q->id;
    app_bump_table[q->id].q.tail = 0;
    app_bump_table[q->id].q.off = q->off;
    app_bump_table[q->id].q.size = q->size;
    app_bump_table[q->id].q.entries = ctx->proto->shm_base + q->off;
    
    if (offs_table[MTYPE_APP_BUMPQ].tail == ID_INVALID)
      offs_table[MTYPE_APP_BUMPQ].head = q->id;
    else
      app_bump_table[offs_table[MTYPE_APP_BUMPQ].tail].next_id = q->id;
      
    offs_table[MTYPE_APP_BUMPQ].tail = q->id;
    
    /* Create queue for bumps app->fast */
    q = cham_new_queue(ctx->proto, 16384);
    if (q == NULL)
    {
      LOG_ERROR("failed to create queue app->fast core=%d", i);
      goto error_uxsocket;
    }
    aev->app_res.af_len = q->size;
    aev->app_res.af_offs[i] = q->off;
    actx->fast_bump_qs[i] = q;

    /* We need to add these queues to the bump table so
       the fast-path can dequeue bumps from the app */
    fast_bump_table[q->id].id = q->id;
    fast_bump_table[q->id].q.head = 0;
    fast_bump_table[q->id].q.off = q->off;
    fast_bump_table[q->id].q.size = q->size;
    fast_bump_table[q->id].q.entries = ctx->proto->shm_base + q->off;

    if (offs_table[MTYPE_FAST_BUMPQ].tail == ID_INVALID)
      offs_table[MTYPE_FAST_BUMPQ].head = q->id;
    else
      fast_bump_table[offs_table[MTYPE_FAST_BUMPQ].tail].next_id = q->id;
      
    offs_table[MTYPE_FAST_BUMPQ].tail = q->id;

    MEM_BARRIER();

    /* Enable queue so fast-path can poll it */
    /* TODO: Now it is just enabled on core 0 but we should load balance it */
    cham_enable_queue(ctx->proto, q->id, 0);
  }
    
  /* Initialise rest of app ctx */
  actx->id = aev->app->n_ctxs;
  actx->app = aev->app;
  aev->app->n_ctxs++;
  aev->app_res.n_fp_cores = ctx->proto->n_fp_cores;
  aev->app_res.shm_len = ctx->proto->shm_size;
  
  /* Send out response */
  res_sz = sizeof(struct udp_queue_new_actx_res);
  n = send(aev->fd, &aev->app_res, res_sz, 0);
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
    uxsocket_error(ctx, aev);
}

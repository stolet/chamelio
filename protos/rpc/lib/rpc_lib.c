#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <errno.h>
#include <fcntl.h>

#include <cham_lib.h>

#include "rpc_lib.h"
#include "queue_fns.h"
#include "rpc_queue_types.h"
#include "log.h"
#include "uxsocket.h"

#define POLL_BATCH 16

static struct rpc_lib *rpc = NULL;

int rpc_connect_slow()
{
  struct rpc_lib *u;
  struct sockaddr_un s_un;
  int shm_fd, ret, sock_fd;
  int64_t tmp;

  if (rpc != NULL)
  {
    LOG_WARN("library already connected to rpc slow-path");
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

  u = malloc(sizeof(struct rpc_lib));
  if (u == NULL)
  {
    LOG_ERROR("failed to allocate rpc_lib struct");
    goto close_sockfd;
  }

  u->uxsocket_fd = sock_fd;
  u->shm_fd = shm_fd;
  u->shm_base = NULL;
  u->next_ctxid = 0;
  rpc = u;
  
  return 0;

close_sockfd:
  close(sock_fd);
  return -1;
}

struct rpc_context_lib * rpc_ctx_new()
{
  int i;
  ssize_t sz, off;
  void *shm_base;
  struct rpc_context_lib *ctx;
  struct rpc_queue_new_actx_res *res;
  __u8 resp_buf[sizeof(*res)];
  struct equeue *eq, **eq_list;
  struct dqueue *dq, **dq_list;
  struct rpc_queue_new_actx_req req = {
    .req = 1,
  };
  
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

  sz = sendmsg(rpc->uxsocket_fd, &msg, 0);
  if (sz != sizeof(req))
  {
    LOG_ERROR("failed to send msg to register rpc app ctx");
    perror("");
    return NULL;
  }

  /* Receive response on kernel socket */
  res = (struct rpc_queue_new_actx_res *) resp_buf;
  off = 0;
  while (off < sizeof(*res)) 
  {
    sz = read(rpc->uxsocket_fd, (__u8 *) res + off, sizeof(*res) - off);
    if (sz < 0) 
    {
      LOG_ERROR("read failed");
      perror("");
      return NULL;
    }
    off += sz;
  }
  
  ctx = malloc(sizeof(struct rpc_context_lib));
  if (ctx == NULL)
  {
    LOG_ERROR("failed to allocate rpc context struct");
    return NULL;
  }
  
  ctx->id = __sync_fetch_and_add(&rpc->next_ctxid, 1);
  ctx->ncores = res->n_fp_cores;
  
  /* Map shared memory if this is the first context */
  if (ctx->id == 0)
  {
    shm_base = mmap(NULL, res->shm_len, PROT_READ | PROT_WRITE, 
        MAP_SHARED | MAP_POPULATE, rpc->shm_fd, 0);
    if (shm_base == (void *) -1) 
    {
      LOG_ERROR("failed to map shm region");
      return NULL;;
    }
    rpc->shm_base = shm_base;
  }
  
  /* Wait until shm_base is mapped */
  while (rpc->shm_base == NULL) {}

  /* Set queue from app to slow-path */
  eq = equeue_new(res->as_nelems, res->as_elsize,
      rpc->shm_base + res->as_off, res->as_off);
  if (eq == NULL)
  {
    LOG_ERROR("failed to create queue from app to slow-path");
    return NULL;
  }
  ctx->app_slow_q = eq;
  
  /* Set queue from slow-path to app */
  dq = dqueue_new(res->sa_nelems, 
      res->sa_elsize,rpc->shm_base + res->sa_off, res->sa_off);
  if (dq == NULL)
  {
    LOG_ERROR("failed to create queue from slow-path to app");
    return NULL;
  }
  ctx->slow_app_q = dq;

  /* Allocate list for bump queues between app and fast-path */
  eq_list = malloc(sizeof(struct equeue *) * res->n_fp_cores);
  if (eq_list == NULL)
  {
    LOG_ERROR("failed to allocate list for queues app->fast");
    return NULL;
  }
  ctx->app_fast_qs = eq_list;

  dq_list = malloc(sizeof(struct dqueue *) * res->n_fp_cores);
  if (dq == NULL)
  {
    LOG_ERROR("failed to allcoate list for queues fast->app");
    return NULL;
  }
  ctx->fast_app_qs = dq_list;

  /* Create each queue between app and fast-path core */
  for (i = 0; i < res->n_fp_cores; i++)
  {
    eq = equeue_new(res->af_nelems, res->af_elsize,
        rpc->shm_base + res->af_offs[i], res->af_offs[i]);
    if (eq == NULL)
    {
      LOG_ERROR("failed to create app->fast bump queue");
      return NULL;
    }
    eq_list[i] = eq;

    dq = dqueue_new(res->fa_nelems, res->fa_elsize,
        rpc->shm_base + res->fa_offs[i], res->fa_offs[i]);
    if (dq == NULL)
    {
      LOG_ERROR("failed to create fast->app bump queue");
      return NULL;
    }
    dq_list[i] = dq;
  }

  return ctx;
}


int rpc_poll_slow(struct rpc_context_lib *ctx)
{
  return 0;
}

int rpc_poll_calls(struct rpc_context_lib *ctx)
{
  return 0;
}

struct rpc_client_lib * rpc_new_client(__u32 ip, __u16 port)
{
  return NULL;
}

struct rpc_server_lib * rpc_new_server(__u32 ip, __u16 port)
{
  return NULL;
}

struct rpc_worker_lib * rpc_new_worker(struct rpc_server_lib *s)
{
  return NULL;
}

int rpc_register(struct rpc_server_lib *server, __u8 service)
{
  return 0;
}

int rpc_call(struct rpc_client_lib *c, __u32 ip, __u16 port,
    __u16 service, void *buf, size_t len)
{
  return 0;
}

int rpc_return(struct rpc_server_lib *s, __u32 ip, __u16 port,
    __u32 rid, void *buf, size_t len)
{
  return 0;
}

int rpc_handle_call(struct rpc_worker_lib *w, 
    void *buf, size_t len)
{
  return 0;
}

int rpc_call_complete(struct rpc_worker_lib *w)
{
  return 0;
}
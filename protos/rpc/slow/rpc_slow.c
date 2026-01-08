#include <stdlib.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <cham_lib.h>
#include <assert.h>

#include "appif.h"
#include "rpc_slow.h"
#include "rpc_queue_types.h"
#include "queue_fns.h"
#include "rpc.h"
#include "log.h"
#include <stdio.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>

int handle_new_sock_rpc(struct rpc_slow_context *ctx, 
  struct rpc_app_context_slow *actx, struct rpc_queue_entry *qe);
int handle_bind_rpc(struct rpc_slow_context *ctx, 
    struct rpc_app_context_slow *actx, struct rpc_queue_entry *qe_req);

int init_rpc_slow_context(struct rpc_slow_context *ctx);

int poll_apps(struct rpc_slow_context *ctx);
    
int init_rpc_slow_context(struct rpc_slow_context *ctx)
{
  int fd, ret;
  struct stat statbuf;
  struct proto_ebpf_lib *ebpf;
  __u8 *ebpf_bytecode;
  struct guest_lib *g;
  struct proto_lib *p;

  g = cham_connect_guest();
  if (g == NULL)
  {
    LOG_ERROR("UDP slow-path couldn't connect to Chamelio");
    abort();
  }

  p = cham_new_proto(g, 0);
  if (p == NULL)
  {
    LOG_ERROR("UDP slow-path failed to register protocol with Chamelio");
    abort();
  }
  
  fd = open(RPC_EBPF_BYTECODE, O_RDWR);
  if (fd < 0)
  {
    LOG_ERROR("Failed to open UDP eBPF bytecode");
    abort();
  }
  fstat(fd, &statbuf);
  
  ebpf = cham_allocate_ebpf(p, statbuf.st_size);
  if (ebpf == NULL)
  {
    LOG_ERROR("Failed to allocate space for eBPF bytecode in shared memory");
    abort();
  }
  
  ebpf_bytecode = mmap(NULL, ebpf->size, PROT_READ | PROT_EXEC,
      MAP_PRIVATE, fd, 0);
  if (ebpf_bytecode == NULL)
  {
    LOG_ERROR("failed to mmap eBPF bytecode");
    abort();
  }
  
  ret = cham_upload_ebpf(p, ebpf_bytecode, ebpf->size);
  if (ret != 0)
  {
    LOG_ERROR("failed to upload eBPF bytecode to shared memory");
    abort();
  }

  ctx->app_uxfd = -1;
  ctx->app_epfd = -1;
  ctx->guest = g;
  ctx->proto = p;
  ctx->n_apps = 0;
  ctx->next_app = 0;

  return 0;
}

int poll_apps(struct rpc_slow_context *ctx)
{
  int msgs_i, apps_polled, ctxs_polled;
  __u8 type;
  struct dqueue *q;
  struct rpc_queue_entry *qe;
  struct rpc_app_slow *a;
  struct rpc_app_context_slow *actx;
 
  msgs_i = 0;
  apps_polled = 0;
  ctxs_polled = 0;
  while (msgs_i < BATCH_SIZE && ctx->n_apps != 0)
  {     
    a = &ctx->apps[ctx->next_app];
    if (ctxs_polled >= a->n_ctxs)
    {
      apps_polled++;
      ctx->next_app = (ctx->next_app + 1) % ctx->n_apps;
      a = &ctx->apps[ctx->next_app];
    }
  
    if (apps_polled >= ctx->n_apps || a->n_ctxs == 0)
    {
      break;
    }
    
    actx = &a->ctxs[a->next_ctx];
    q = actx->app_slow_q;
    qe = queue_head(q);
    
    if (qe == NULL)
    {
      ctxs_polled++;
      a->next_ctx = (a->next_ctx + 1) % a->n_ctxs;
      continue;
    }

    msgs_i++;
    type = qe->type;
    switch (type)
    {
      case RPC_QUEUE_EMPTY:
        break;
      case RPC_QUEUE_NEW_SOCK_REQ:
        handle_new_sock_rpc(ctx, actx, qe);
        break;
      case RPC_QUEUE_BIND_REQ:
        handle_bind_rpc(ctx, actx, qe);
        break;
      default:
        LOG_WARN("unknown queue entry type from app " 
          "to udp slow-path type=%d", type);
          break;
    }
    queue_dequeue(q);
  }

  return 0;
}

int poll_control(struct rpc_slow_context *ctx)
{
  return 0;
}

int main(int argc, char **argv)
{
  int ret;
  struct rpc_slow_context ctx;
  
  ret = init_rpc_slow_context(&ctx);
  if (ret != 0)
  {
    LOG_ERROR("failed to initialise udp slow context");
    abort();
  }
  
  ret = appif_init(&ctx);
  if (ret != 0)
  {
    LOG_ERROR("failed to initialise appiff");
    abort();
  }
  
  while (1)
  {
    appif_poll(&ctx);
    poll_apps(&ctx);
  }
}

int handle_new_sock_rpc(struct rpc_slow_context *ctx, 
    struct rpc_app_context_slow *actx, struct rpc_queue_entry *qe_req)
{
  int ret;

  // ret = cham_new_sock(actx->app_fd, qe_req->sock);
  // if (ret != 0)
  // {
  //   LOG_ERROR("failed to create new socket");
  //   return ret;
  // }

  return 0;
}

int handle_bind_rpc(struct rpc_slow_context *ctx, 
    struct rpc_app_context_slow *actx, struct rpc_queue_entry *qe_req)
{
  int ret;

  // ret = cham_bind_sock(actx->app_fd, qe_req->bind_req.sock,
  //     qe_req->bind_req.ip, qe_req->bind_req.port);
  // if (ret != 0)
  // {
  //   LOG_ERROR("failed to bind socket");
  //   return ret;
  // }

  return 0;
}

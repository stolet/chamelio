#include <stdlib.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <cham_lib.h>
#include <assert.h>

#include "appif.h"
#include "udp_slow.h"
#include "udp_queue_types.h"
#include "queue_fns.h"
#include "udp.h"
#include "udp_state.h"
#include "log.h"
#include <stdio.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include "udp_config.h"

int init_udp_slow_context(struct udp_slow_context *ctx);

int poll_apps(struct udp_slow_context *ctx);
static void udp_state_publish(struct udp_slow_context *ctx);

int handle_new_sock(struct udp_slow_context *ctx, 
  struct udp_app_context_slow *actx, struct udp_queue_entry *qe);
int handle_bind(struct udp_slow_context *ctx, 
    struct udp_app_context_slow *actx, struct udp_queue_entry *qe_req);
int handle_sock_setopt(struct udp_slow_context *ctx, 
    struct udp_app_context_slow *actx, struct udp_queue_entry *qe_req);
static __u16 find_free_port(struct udp_slow_context *ctx);
   
int init_udp_slow_context(struct udp_slow_context *ctx)
{
  int fd, ret, i, j;
  struct stat statbuf;
  struct proto_ebpf_lib *ebpf;
  __u8 *ebpf_bytecode;
  struct guest_lib *g;
  struct proto_lib *p;
  struct proto_map_lib *cfg_map, *port_map, *socks_map;
  struct udp_cfg *cfg;
  struct udp_port *ports;

  if (!ctx->config.virt)
  {
    g = cham_connect_guest();
    if (g == NULL)
    {
      LOG_ERROR("UDP slow-path couldn't connect to Chamelio");
      abort();
    }

    p = cham_new_proto_bare(g, CHAM_PROTO_UDP);
    if (p == NULL)
    {
      LOG_ERROR("UDP slow-path failed to register protocol with Chamelio");
      abort();
    }
  }
  else
  {
    p = cham_new_proto_virt(CHAM_PROTO_UDP);
    if (p == NULL)
    {
      LOG_ERROR("UDP slow-path failed to register protocol with Chamelio");
      abort();
    }
  }
  
  fd = open(ctx->config.ebpf_path, O_RDWR);
  if (fd < 0)
  {
    LOG_ERROR("Failed to open UDP eBPF bytecode at %s", ctx->config.ebpf_path);
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

  /* Create map used to hold local port to sockets translation */
  port_map = cham_new_map(p, MAX_PORT, sizeof(struct udp_port));
  if (port_map == NULL)
  {
    LOG_ERROR("failed to create map for port to socket translation");
    abort();
  }
  ctx->port_map = port_map;

  /* Create map used to hold sockets */
  socks_map = cham_new_map(p, MAX_SOCKETS, sizeof(struct udp_sock));
  if (socks_map == NULL)
  {
    LOG_ERROR("failed to create map to hold sockets");
    abort();
  }
  ctx->socks_map = socks_map;

  cfg_map = cham_new_map(p, 1, sizeof(struct udp_cfg));
  if (cfg_map == NULL)
  {
    LOG_ERROR("failed to create UDP config map");
    abort();
  }
  ctx->cfg_map = cfg_map;

  /* Initialize entries in port_map */
  ports = p->shm_base + port_map->off;
  for (i = 0; i < MAX_SOCKETS; i++)
  {
    ports[i].nsocks = 0;
    for (j = 0; j < MAX_FP_CORES; j++)
      ports[i].next_sock[j] = j;
  }

  cfg = p->shm_base + cfg_map->off;
  cfg->next_port = MIN_PORT;

  ctx->app_uxfd = -1;
  ctx->app_epfd = -1;
  ctx->proto = p;
  ctx->n_apps = 0;
  ctx->next_app = 0;
  ctx->n_socks = 0;

  LOG_INFO("UDP initialized");
  return 0;
}

int poll_apps(struct udp_slow_context *ctx)
{
  int msgs_i, apps_polled, ctxs_polled;
  __u8 type;
  struct dqueue *q;
  struct udp_queue_entry *qe;
  struct udp_app_slow *a;
  struct udp_app_context_slow *actx;
 
  msgs_i = 0;
  apps_polled = 0;
  ctxs_polled = 0;
  while (msgs_i < SLOW_BATCH_SIZE && ctx->n_apps != 0)
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
      case UDP_QUEUE_EMPTY:
        break;
      case UDP_QUEUE_NEW_SOCK_REQ:
        handle_new_sock(ctx, actx, qe);
        break;
      case UDP_QUEUE_SETOPT_REQ:
        handle_sock_setopt(ctx, actx, qe);
        break;
      case UDP_QUEUE_BIND_REQ:
        handle_bind(ctx, actx, qe);
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

int poll_control(struct udp_slow_context *ctx)
{
  return 0;
}

int handle_new_sock(struct udp_slow_context *ctx, 
    struct udp_app_context_slow *actx, struct udp_queue_entry *qe_req)
{
  int ret;
  struct udp_sock *sock;
  struct proto_queue_lib *protoq;
  struct udp_queue_entry *qe_res;
  struct udp_queue_new_sock_req *req;
  struct udp_queue_new_sock_res *res;
  __u32 i;
  __u16 core;

  struct udp_sock *socks_map = ctx->proto->shm_base + ctx->socks_map->off;

  if (ctx->n_socks >= MAX_SOCKETS)
  {
    LOG_ERROR("Socket map is full");
    return -1;
  }

  req = &qe_req->data.new_sock_req;
  qe_res = queue_tail(actx->slow_app_q);
  if (qe_res == NULL)
  {
    LOG_ERROR("failed to get tail of slow->app queue");
    return -1;
  }
  res = &qe_res->data.new_sock_res;
  res->opaque = req->opaque;
  res->sock_id = ctx->n_socks;
  core = 0;
  if (ctx->proto->n_fp_cores != 0)
    core = ctx->n_socks % ctx->proto->n_fp_cores;
  res->core = core;

  sock = &socks_map[res->sock_id];
  sock->id = res->sock_id;
  sock->tx_core = core;
  sock->rx_core = -1;
  sock->local_ip = ctx->proto->local_ip;
  sock->app_bump_qid = actx->app_bump_qs[core]->id;
  for (i = 0; i < ctx->proto->n_fp_cores; i++)
    sock->app_bump_qids[i] = actx->app_bump_qs[i]->id;
  sock->opaque = req->opaque;
  sock->local_port = 0;
  sock->reuport = 0;

  /* Create queue for RX buffer */
  protoq = cham_new_queue(ctx->proto, ctx->config.rxbuf_sz, 1);
  if (protoq == NULL)
  {
    LOG_ERROR("failed to create new queue");
    return -1;
  }
  res->rx_qid = protoq->id;
  res->rx_len = protoq->elsize * protoq->nelems;
  res->rx_off = protoq->off;
  sock->rx_len = protoq->elsize * protoq->nelems;
  sock->rx_avail = 0;
  sock->rx_head = 0;
  sock->rx_off = protoq->off;

  /* Create queue for TX buffer */
  protoq = cham_new_queue(ctx->proto, ctx->config.txbuf_sz, 1);
  res->tx_qid = protoq->id;
  res->tx_len = protoq->nelems * protoq->elsize;
  res->tx_off = protoq->off;
  sock->tx_len = protoq->nelems * protoq->elsize;
  sock->tx_avail = 0;
  sock->tx_head = 0;
  sock->tx_off = protoq->off;

  /* Send response to app */
  ret = queue_enqueue(actx->slow_app_q, UDP_QUEUE_NEW_SOCK_RES);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue new socket response");
    return -1;
  }

  /* Increment number of socks registered in protocol */
  ctx->n_socks++;

  return 0;
}

int handle_sock_setopt(struct udp_slow_context *ctx, 
    struct udp_app_context_slow *actx, struct udp_queue_entry *qe_req)
{
  int ret;
  struct udp_sock *sock;
  struct udp_queue_entry *qe_res;
  struct udp_queue_setopt_req *req;
  struct udp_queue_setopt_res *res;
  struct udp_sock *socks_map = ctx->proto->shm_base + ctx->socks_map->off;
  
  req = &qe_req->data.setopt_req;
  qe_res = queue_tail(actx->slow_app_q);
  if (qe_res == NULL)
  {
    LOG_ERROR("faied to get tail of slow->app queue");
    return -1;
  }
  res = &qe_res->data.setopt_res;
  
  if (req->sock_id < 0 || req->sock_id > MAX_SOCKETS)
  {
    LOG_ERROR("invalid socket id");
    res->success = 0;
    res->opaque = req->opaque;
    ret = queue_enqueue(actx->slow_app_q, UDP_QUEUE_SETOPT_RES);
    if (ret != 0)
    {
      LOG_ERROR("failed to enqueue UDP queue bind response");
      return -1;
    }
  }
  
  sock = &socks_map[req->sock_id];
  switch (req->opt)
  {
    case SO_REUSEPORT:
      sock->reuport = 1;
      break;
    default:
      break;
  }
  
  res->success = 1;
  res->opaque = req->opaque;
  ret = queue_enqueue(actx->slow_app_q, UDP_QUEUE_SETOPT_RES);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue UDP queue bind response");
    return -1;
  }

  return 0;
  
}

int handle_bind(struct udp_slow_context *ctx, 
    struct udp_app_context_slow *actx, struct udp_queue_entry *qe_req)
{
  int ret;
  __u16 local_port;
  struct udp_port *port_map, *port;
  struct udp_queue_entry *qe_res;
  struct udp_queue_bind_req *req;
  struct udp_queue_bind_res *res;
  struct udp_sock *sock, *socks_map;

  qe_res = queue_tail(actx->slow_app_q);
  if (qe_res == NULL)
  {
    LOG_ERROR("failed to get tail of slow->app queue");
    return -1;
  }
  
  req = &qe_req->data.bind_req;
  socks_map = ctx->proto->shm_base + ctx->socks_map->off;
  sock = &socks_map[req->sock_id];
  local_port = req->local_port;
  if (sock->local_port != 0)
    local_port = sock->local_port;
  
  /* Return error if port is invalid */
  res = &qe_res->data.bind_res;
  if (local_port > MAX_PORT)
  {
    LOG_ERROR("port is invalid");
    res->success = 0;
    res->opaque = req->opaque;
    ret = queue_enqueue(actx->slow_app_q, UDP_QUEUE_BIND_RES);
    if (ret != 0)
      LOG_ERROR("failed to enqueue UDP queue bind response");
    return -1;
  }
  
  /* Choose a port if port is 0 */
  if (local_port == 0)
  {
    local_port = find_free_port(ctx);
    if (local_port == 0)
    {
      LOG_ERROR("failed to find free UDP port");
      res->success = 0;
      res->opaque = req->opaque;
      ret = queue_enqueue(actx->slow_app_q, UDP_QUEUE_BIND_RES);
      if (ret != 0)
        LOG_ERROR("failed to enqueue UDP queue bind response");
      return -1;
    }
  }
  
  /* Return error if another socket is already using this port */
  port_map = ctx->proto->shm_base + ctx->port_map->off;
  port = &port_map[local_port];
  if (port->nsocks != 0 && !sock->reuport)
  {
    LOG_ERROR("socket with this port already in use port=%d", port);
    res->success = 0;
    res->opaque = req->opaque;
    ret = queue_enqueue(actx->slow_app_q, UDP_QUEUE_BIND_RES);
    if (ret != 0)
      LOG_ERROR("failed to enqueue UDP queue bind response");
    return -1;
  }
  if (port->nsocks >= MAX_REUSOCK_PORT)
  {
    LOG_ERROR("too many sockets bound to UDP port");
    res->success = 0;
    res->opaque = req->opaque;
    ret = queue_enqueue(actx->slow_app_q, UDP_QUEUE_BIND_RES);
    if (ret != 0)
      LOG_ERROR("failed to enqueue UDP queue bind response");
    return -1;
  }

  sock->local_ip = req->local_ip;
  sock->local_port = local_port;

  port->sids[port->nsocks] = req->sock_id;
  port->nsocks++;

  res->success = 1;
  res->opaque = req->opaque;
  ret = queue_enqueue(actx->slow_app_q, UDP_QUEUE_BIND_RES);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue UDP queue bind response");
    return -1;
  }

  return 0;
}

int main(int argc, char **argv)
{
  int ret;
  struct udp_slow_context ctx;
  
  ret = udp_config_parse(&ctx.config, argc, argv);
  if (ret != 0)
  {
    LOG_ERROR("failed to parse UDP configuration");
    abort();
  }
  
  ret = init_udp_slow_context(&ctx);
  if (ret != 0)
  {
    LOG_ERROR("failed to initialise udp slow context");
    abort();
  }

  udp_state_publish(&ctx);
  
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

static void udp_state_publish(struct udp_slow_context *ctx)
{
  int fd;
  struct udp_state state = {
    .magic = UDP_STATE_MAGIC,
    .version = UDP_STATE_VERSION,
    .pid = getpid(),
    .ctx_addr = (__u64) ctx,
    .ctx_size = sizeof(*ctx),
    .sock_size = sizeof(struct udp_sock),
    .port_size = sizeof(struct udp_port),
    .cfg_size = sizeof(struct udp_cfg),
    .app_size = sizeof(struct udp_app_slow),
    .app_ctx_size = sizeof(struct udp_app_context_slow),
  };

  mkdir("/run/chamelio", 0777);
  fd = open(UDP_STATE_PATH, O_CREAT | O_WRONLY | O_TRUNC, 0666);
  if (fd < 0)
  {
    LOG_WARN("failed to publish UDP state");
    return;
  }

  if (write(fd, &state, sizeof(state)) != sizeof(state))
    LOG_WARN("failed to write UDP state");
  close(fd);
}

static __u16 find_free_port(struct udp_slow_context *ctx)
{
  __u16 i, nr, next_port;
  struct udp_cfg *cfg;
  struct udp_port *port;

  cfg = ctx->proto->shm_base + ctx->cfg_map->off;
  if (cfg == NULL)
    return 0;

  next_port = cfg->next_port;
  if (next_port < MIN_PORT || next_port > MAX_PORT)
    next_port = MIN_PORT;

  port = ctx->proto->shm_base + ctx->port_map->off;
  for (i = 0; i < UDP_PORT_SCAN_MAX; i++)
  {
    nr = next_port + i;
    if (nr > MAX_PORT)
      nr = MIN_PORT + nr - MAX_PORT - 1;

    if (port[nr].nsocks == 0)
    {
      cfg->next_port = nr + 1;
      if (cfg->next_port > MAX_PORT)
        cfg->next_port = MIN_PORT;
      return nr;
    }
  }

  return 0;
}

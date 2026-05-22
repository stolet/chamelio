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
#include <string.h>

int handle_new_client_req(struct rpc_slow_context *ctx,
                          struct rpc_app_context_slow *actx, struct rpc_queue_entry *qe);

int handle_new_server_req(struct rpc_slow_context *ctx,
                          struct rpc_app_context_slow *actx, struct rpc_queue_entry *qe);

int handle_new_worker_req(struct rpc_slow_context *ctx,
                          struct rpc_app_context_slow *actx, struct rpc_queue_entry *qe);

int handle_new_service_req(struct rpc_slow_context *ctx,
                           struct rpc_app_context_slow *actx, struct rpc_queue_entry *qe);

int init_rpc_slow_context(struct rpc_slow_context *ctx);

int poll_apps(struct rpc_slow_context *ctx);

int init_rpc_slow_context(struct rpc_slow_context *ctx)
{
  int fd, ret, i;
  struct stat statbuf;
  struct proto_ebpf_lib *ebpf;
  __u8 *ebpf_bytecode;
  struct guest_lib *g;
  struct proto_lib *p;
  struct proto_map_lib *pt_map, *servers_map, *workers_map, *clients_map;
  struct rpc_port_entry *ports;

  g = cham_connect_guest();
  if (g == NULL)
  {
    LOG_ERROR("RPC slow-path couldn't connect to Chamelio");
    abort();
  }

  p = cham_new_proto(g, 0);
  if (p == NULL)
  {
    LOG_ERROR("RPC slow-path failed to register protocol with Chamelio");
    abort();
  }

  fd = open(RPC_EBPF_BYTECODE, O_RDWR);
  if (fd < 0)
  {
    LOG_ERROR("Failed to open RPC eBPF bytecode");
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

  clients_map = cham_new_map(p, MAX_CLIENTS, sizeof(struct rpc_client));
  if (clients_map == NULL)
  {
    LOG_ERROR("failed to create map to hold clients");
    abort();
  }
  servers_map = cham_new_map(p, MAX_SERVERS, sizeof(struct rpc_server));
  if (servers_map == NULL)
  {
    LOG_ERROR("failed to create map to hold servers");
    abort();
  }
  workers_map = cham_new_map(p, MAX_WORKERS, sizeof(struct rpc_worker));
  if (workers_map == NULL)
  {
    LOG_ERROR("failed to create map to hold workers");
    abort();
  }
  pt_map = cham_new_map(p, MAX_PORT + 1, sizeof(struct rpc_port_entry));
  if (pt_map == NULL)
  {
    LOG_ERROR("failed to create port to server map");
    abort();
  }
  ctx->port_map = pt_map;

  ctx->clients_map = clients_map;
  ctx->servers_map = servers_map;
  // ctx->port_server_map = pt_to_ser_map;
  ctx->workers_map = workers_map;
  LOG_DEBUG("initialized rpc slow context maps");
  ctx->app_uxfd = -1;
  ctx->app_epfd = -1;
  ctx->guest = g;
  ctx->proto = p;
  ctx->n_apps = 0;
  ctx->next_app = 0;
  ctx->n_socks = 0;
  ctx->n_clients = 0;
  ctx->n_servers = 0;
  ctx->n_workers = 0;

  // initialize port to server map entries to invalid server id
  ports = ctx->proto->shm_base + ctx->port_map->off;
  for (i = MIN_PORT; i <= MAX_PORT; i++)
  {
    ports[i].server_id = INVALID_ID;
    ports[i].client_id = INVALID_ID;
  }
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
    case RPC_QUEUE_NEW_CLIENT_REQ:
      handle_new_client_req(ctx, actx, qe);
      break;
    case RPC_QUEUE_NEW_SERVER_REQ:
      LOG_DEBUG("handling new server request");
      handle_new_server_req(ctx, actx, qe);
      LOG_DEBUG("new server request handled successfully");
      break;
    case RPC_QUEUE_NEW_WORKER_REQ:
      handle_new_worker_req(ctx, actx, qe);
      break;
    case RPC_QUEUE_SERVICE_REQ:
      handle_new_service_req(ctx, actx, qe);
      break;
    // case RPC_QUEUE_BIND_REQ:
    //   handle_bind_rpc(ctx, actx, qe);
    //   break;
    default:
      LOG_WARN("unknown queue entry type from app "
               "to udp slow-path type=%d",
               type);
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

int handle_new_client_req(struct rpc_slow_context *ctx,
                          struct rpc_app_context_slow *actx, 
                          struct rpc_queue_entry *qe_req)
{
  int ret;
  struct rpc_client *cl;
  struct rpc_queue_new_client_req *req;
  struct rpc_queue_new_client_res *res;
  struct rpc_queue_entry *qe_res;
  struct proto_queue_lib *protoq;
  struct rpc_port_entry *pt_cl_map, *port;

  struct rpc_client *client = ctx->proto->shm_base + ctx->clients_map->off;
  pt_cl_map = ctx->proto->shm_base + ctx->port_map->off;

  if (ctx->n_clients >= MAX_CLIENTS)
  {
    LOG_ERROR("maximum number of rpc clients reached");
    return -1;
  }

  req = &qe_req->data.new_client_req;
  qe_res = queue_tail(actx->slow_app_q);
  if (qe_res == NULL)
  {
    LOG_ERROR("failed to get tail of slow->app queue");
    return -1;
  }
  res = &qe_res->data.new_client_res;
  res->opaque = req->opaque;
  res->core = 0;
  res->client_id = ctx->n_clients;

  cl = &client[res->client_id];
  memset(cl, 0, sizeof(struct rpc_client));
  cl->id = res->client_id;
  cl->core = 0;
  cl->app_bump_qid = actx->app_bump_qs[0]->id;
  cl->opaque = req->opaque;
  cl->local_ip = req->local_ip;

  if (req->local_port != 0)
  {
    port = &pt_cl_map[req->local_port];
    if (port->client_id != (__u32)INVALID_ID || port->server_id != (__u32)INVALID_ID)
    {
      LOG_ERROR("port already bound to another client or server");
      res->success = 0;
      ret = queue_enqueue(actx->slow_app_q, RPC_QUEUE_NEW_CLIENT_RES);
      if (ret != 0)
      {
        LOG_ERROR("failed to enqueue rpc new client response");
      }
      return -1;
    }
    cl->local_port = req->local_port;
  }

  port = &pt_cl_map[cl->local_port];
  port->client_id = res->client_id;

  // Create queue for RX buffer
  protoq = cham_new_queue(ctx->proto, RXBUF_SZ, 1);
  if (protoq == NULL)
  {
    LOG_ERROR("failed to create new queue");
    return -1;
  }
  res->rx_qid = protoq->id;
  res->rx_len = protoq->nelems * protoq->elsize;
  res->rx_off = protoq->off;

  cl->rx_len = protoq->nelems * protoq->elsize;
  cl->rx_off = protoq->off;

  // Create queue for TX buffer

  protoq = cham_new_queue(ctx->proto, TXBUF_SZ, 1);
  if (protoq == NULL)
  {
    LOG_ERROR("failed to create new queue");
    return -1;
  }
  res->tx_qid = protoq->id;
  LOG_DEBUG("proto queue elsize=%d nelems=%d", protoq->elsize, protoq->nelems);
  res->tx_len = protoq->nelems * protoq->elsize;
  res->tx_off = protoq->off;

  cl->tx_len = protoq->nelems * protoq->elsize;
  cl->tx_off = protoq->off;

  res->success = 1;
  LOG_DEBUG("new client request handled successfully, client id=%d", res->client_id);

  ret = queue_enqueue(actx->slow_app_q, RPC_QUEUE_NEW_CLIENT_RES);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue rpc new client response");
    return -1;
  }
  ctx->n_clients++;
  return 0;
}

int handle_new_server_req(struct rpc_slow_context *ctx,
                          struct rpc_app_context_slow *actx, struct rpc_queue_entry *qe_req)
{
  LOG_DEBUG("handling new server request");
  int ret;
  struct rpc_queue_new_server_req *req;
  struct rpc_queue_new_server_res *res;
  struct rpc_queue_entry *qe_res;
  struct rpc_server *sv;
  struct proto_queue_lib *protoq;
  struct rpc_port_entry *pt_sers_map, *port;
  struct rpc_server *server_map = ctx->proto->shm_base + ctx->servers_map->off;

  pt_sers_map = ctx->proto->shm_base + ctx->port_map->off;

  if (ctx->n_servers >= MAX_SERVERS)
  {
    LOG_ERROR("maximum number of rpc servers reached");
    return -1;
  }
  req = &qe_req->data.new_server_req;
  qe_res = queue_tail(actx->slow_app_q);
  if (qe_res == NULL)
  {
    LOG_ERROR("failed to get tail of slow->app queue");
    return -1;
  }
  res = &qe_res->data.new_server_res;
  res->opaque = req->opaque;
  res->core = 0;
  res->server_id = ctx->n_servers;

  sv = &server_map[res->server_id];
  res->server_off = ctx->servers_map->off +
                  ((__u64)res->server_id * sizeof(struct rpc_server));
  memset(sv, 0, sizeof(struct rpc_server));

  sv->id = res->server_id;
  sv->local_ip = req->local_ip;
  sv->local_port = req->local_port;
  sv->n_workers = 0;

  // initialize workers IDs table to invalid id
  for (int i = 0; i < MAX_WORKERS; i++)
  {
    sv->workers[i] = INVALID_ID;
  }
  // initialize service table to 0
  for (int i = 0; i < MAX_SERVICE_NUMBER; i++)
  {
    sv->service_table[i] = 0;
  }

  // bind the port to the server id in the port to server map
  port = &pt_sers_map[req->local_port];
  if (port->server_id != INVALID_ID || port->client_id != INVALID_ID)
  {
    LOG_ERROR("port already bound to another server or client");
    res->success = 0;
    ret = queue_enqueue(actx->slow_app_q, RPC_QUEUE_NEW_SERVER_RES);
    if (ret != 0)
    {
      LOG_ERROR("failed to enqueue rpc new server response");
      return -1;
    }
    return -1;
  }

  port->server_id = res->server_id;

  /* Allocate shared RX ring for app. LB */
  protoq = cham_new_queue(ctx->proto, (size_t)MAX_WORKERS * RXBUF_SZ, 1);
  if (protoq == NULL)
  {
    LOG_ERROR("failed to allocate shared RX ring for server");
    return -1;
  }

  res->rx_len = protoq->nelems * protoq->elsize;
  res->rx_off = protoq->off;

  sv->rx_len = protoq->nelems * protoq->elsize;
  sv->rx_off = protoq->off;

  res->success = 1;
  ret = queue_enqueue(actx->slow_app_q, RPC_QUEUE_NEW_SERVER_RES);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue rpc new server response");
    return -1;
  }
  ctx->n_servers++;
  LOG_DEBUG("new server request handled successfully, server id=%d", res->server_id);
  return 0;
}

int handle_new_worker_req(struct rpc_slow_context *ctx,
                          struct rpc_app_context_slow *actx, struct rpc_queue_entry *qe_req)
{
  int ret;
  struct rpc_worker *worker;
  struct rpc_queue_new_worker_req *req;
  struct rpc_queue_new_worker_res *res;
  struct rpc_queue_entry *qe_res;
  struct proto_queue_lib *protoq;
  struct rpc_server *server;
  struct rpc_server *sv_map = ctx->proto->shm_base + ctx->servers_map->off;
  struct rpc_worker *worker_map = ctx->proto->shm_base + ctx->workers_map->off;

  req = &qe_req->data.new_worker_req;
  qe_res = queue_tail(actx->slow_app_q);
  if (qe_res == NULL)
  {
    LOG_ERROR("failed to get tail of slow->app queue");
    return -1;
  }
  LOG_DEBUG("got tail of slow->app queue");

  res = &qe_res->data.new_worker_res;
  res->opaque = req->opaque;

  LOG_DEBUG("creating new worker for server ID %d", req->server_id);
  server = &sv_map[req->server_id];
  if (server->n_workers >= MAX_WORKERS)
  {
    LOG_ERROR("maximum number of workers for server ID %d reached", server->id);
    return -1;
  }

  res->worker_id = ctx->n_workers;
  worker = &worker_map[ctx->n_workers];
  res->worker_off = ctx->workers_map->off +
                    ((__u64)ctx->n_workers * sizeof(struct rpc_worker));
  memset(worker, 0, sizeof(struct rpc_worker));
  worker->id = ctx->n_workers;
  worker->app_bump_qid = actx->app_bump_qs[0]->id;
  worker->opaque = req->opaque;
  worker->jobs_pending = 0;
  worker->server_id = req->server_id;

  // Create queue for RX buffer
  protoq = cham_new_queue(ctx->proto, RXBUF_SZ, 1);
  if (protoq == NULL)
  {
    LOG_ERROR("failed to create new queue");
    return -1;
  }
  res->rx_qid = protoq->id;
  res->rx_len = protoq->nelems * protoq->elsize;
  res->rx_off = protoq->off;

  worker->rx_len = protoq->nelems * protoq->elsize;
  worker->rx_off = protoq->off;

  // Create queue for TX buffer
  protoq = cham_new_queue(ctx->proto, TXBUF_SZ, 1);
  if (protoq == NULL)
  {
    LOG_ERROR("failed to create new queue");
    return -1;
  }
  res->tx_qid = protoq->id;
  res->tx_len = protoq->nelems * protoq->elsize;
  res->tx_off = protoq->off;
  worker->tx_len = protoq->nelems * protoq->elsize;
  worker->tx_off = protoq->off;

  server->workers[server->n_workers] = worker->id;
  server->n_workers++;

  ret = queue_enqueue(actx->slow_app_q, RPC_QUEUE_NEW_WORKER_RES);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue rpc new worker response");
    return -1;
  }
  ctx->n_workers++;
  LOG_DEBUG("new worker request handled successfully, worker id=%d", res->worker_id);
  return 0;
}

int handle_new_service_req(struct rpc_slow_context *ctx,
                           struct rpc_app_context_slow *actx, struct rpc_queue_entry *qe_req)
{
  int ret;
  struct rpc_queue_service_req *req;
  struct rpc_queue_service_res *res;
  struct rpc_queue_entry *qe_res;
  struct rpc_server *server;
  struct rpc_server *s = ctx->proto->shm_base + ctx->servers_map->off;

  req = &qe_req->data.service_req;
  qe_res = queue_tail(actx->slow_app_q);
  if (qe_res == NULL)
  {
    LOG_ERROR("failed to get tail of slow->app queue");
    return -1;
  }
  res = &qe_res->data.service_res;
  res->opaque = req->opaque;

  server = &s[req->server_id];

  // check the table if the service alreayd exists and return failure if so
  if (server->service_table[req->service_id])
  {
    LOG_ERROR("service ID %d already registered for server ID %d", req->service_id, server->id);
    res->success = 0;
    ret = queue_enqueue(actx->slow_app_q, RPC_QUEUE_SERVICE_RES);
    if (ret != 0)
    {
      LOG_ERROR("failed to enqueue rpc service registration response");
      return -1;
    }
    return -1;
  }
  // register service in the server's service table
  server->service_table[req->service_id] = 1;
  res->success = 1;

  ret = queue_enqueue(actx->slow_app_q, RPC_QUEUE_SERVICE_RES);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue rpc service registration response");
    return -1;
  }

  return 0;
}
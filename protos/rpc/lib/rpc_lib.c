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
#include "rpc_hdr.h"
#include "rpc.h"

#define POLL_BATCH 16


static struct rpc_lib *rpc = NULL;
static __thread struct rpc_context_lib *rpc_thread_ctx = NULL;

static int handle_new_client_res(struct rpc_queue_entry *qe);
static int handle_new_server_res(struct rpc_queue_entry *qe);
static int handle_new_worker_res(struct rpc_queue_entry *qe);
static int handle_new_service_res(struct rpc_queue_entry *qe);
static int handle_tx_bump(struct rpc_queue_bump_entry *qe);
static int handle_rx_bump(struct rpc_queue_bump_entry *qe);
static void ring2ring(__u8 *dst, __u32 dst_pos, __u32 dst_ring_len,
    const __u8 *src, __u32 src_pos, __u32 src_ring_len, __u32 len);
//TODO: put the ring reads in a helper function


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
  u->next_clientid = 0;
  rpc = u;
  u->next_serverid = 0;
  u->next_rid = 0;

  return 0;

close_sockfd:
  close(sock_fd);
  return -1;
}

struct rpc_context_lib *rpc_ctx_new()
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
  res = (struct rpc_queue_new_actx_res *)resp_buf;
  off = 0;
  while (off < sizeof(*res))
  {
    sz = read(rpc->uxsocket_fd, (__u8 *)res + off, sizeof(*res) - off);
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
    if (shm_base == (void *)-1)
    {
      LOG_ERROR("failed to map shm region");
      return NULL;
      ;
    }
    rpc->shm_base = shm_base;
  }

  /* Wait until shm_base is mapped */
  while (rpc->shm_base == NULL)
  {
  }

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
                  res->sa_elsize, rpc->shm_base + res->sa_off, res->sa_off);
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
  if (dq_list == NULL)
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

  rpc_thread_ctx = ctx;
  return ctx;
}

int rpc_poll_slow(struct rpc_context_lib *ctx)
{
  int n;
  struct dqueue *q;
  struct rpc_queue_entry *qe;

  if (!ctx)
  {
    LOG_ERROR("rpc_poll_slow: null rpc context");
    return -1;
  }
  n = 0;
  q = ctx->slow_app_q;
  while (n < POLL_BATCH)
  {
    qe = queue_head(q);
    if (qe == NULL)
      return -1;

    n++;
    switch (qe->type)
    {
    case RPC_QUEUE_NEW_CLIENT_RES:
      handle_new_client_res(qe);
      break;
    case RPC_QUEUE_NEW_SERVER_RES:
      handle_new_server_res(qe);
      break;
    case RPC_QUEUE_NEW_WORKER_RES:
      handle_new_worker_res(qe);
      break;
    case RPC_QUEUE_SERVICE_RES:
      handle_new_service_res(qe);
      break;
    default:
      LOG_ERROR("unknown queue entry type from "
                "slow-path to app type=%d",
                qe->type);
      abort();
    }
    queue_dequeue(q);
  }
  return 0;
}

int rpc_poll_calls(struct rpc_context_lib *ctx)
{
  int i, n, ncores;
  struct dqueue *q;
  struct rpc_queue_bump_entry *qe;
  struct dqueue **dq_list;

  if (!ctx)
  {
    LOG_ERROR("rpc_poll_calls: null rpc context");
    return -1;
  }
  // Poll the call queues for any new messages
  n = 0;
  ncores = ctx->ncores;
  dq_list = ctx->fast_app_qs;

  for (i = 0; i < ncores && n < POLL_BATCH; i++)
  {
    q = dq_list[i];

    while (n < POLL_BATCH && (qe = queue_head(q)) != NULL)
    {
      /* Worker RX bumps stay in the queue — rpc_handle_call peeks at them
         and rpc_return dequeues them once the request is fully handled. */
      if (qe->type == RPC_QUEUE_BUMP_APP_RX &&
          qe->data.bump_app_rx.type == 0)
        break;

      n++;
      switch (qe->type)
      {
      case RPC_QUEUE_BUMP_APP_TX:
        handle_tx_bump(qe);
        break;
      case RPC_QUEUE_BUMP_APP_RX:
        handle_rx_bump(qe);
        break;
      default:
        LOG_ERROR("unknown queue entry type from fast-path to app type=%d",
                  qe->type);
        abort();
      }
      queue_dequeue(q);
    }
  }
  return n;
}

struct rpc_client_lib *rpc_new_client(struct rpc_context_lib *ctx, __u32 ip, __u16 port)
{
  struct rpc_client_lib *client;
  struct equeue *eq;
  struct rpc_queue_entry *qe;
  struct rpc_queue_new_client_req *nc_req;
  // struct rpc_queue_bind_req *bind_req;
  int ret, id;

  if (!ctx || !rpc)
  {
    LOG_ERROR("null rpc context");
    return NULL;
  }

  id = __sync_fetch_and_add(&rpc->next_clientid, 1);
  if (id >= MAX_CLIENTS)
  {
    LOG_ERROR("exceeded max number of rpc clients");
    return NULL;
  }
  client = &rpc->clients[id];
  memset(client, 0, sizeof(struct rpc_client_lib));

  client->ctx = ctx;
  // client->client_id = id;
  client->bind_success = -1;
  // client->type = RPC_ENTITY_CLIENT;

  eq = ctx->app_slow_q;
  qe = queue_tail(eq);
  if (!qe)
  {
    LOG_ERROR("failed to get queue tail for new sock req");
    return NULL;
  }
  nc_req = &qe->data.new_client_req;
  nc_req->local_ip = ip;
  nc_req->local_port = port;
  nc_req->opaque = (__u64)client;
  ret = queue_enqueue(eq, RPC_QUEUE_NEW_CLIENT_REQ);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue new sock req");
    return NULL;
  }

  // poll until we get response
  while (client->bind_success == -1)
    rpc_poll_slow(ctx);
  if (!client->bind_success)
  {
    LOG_ERROR("bind failed for rpc client");
    return NULL;
  }
  return client;
}

struct rpc_server_lib *rpc_new_server(struct rpc_context_lib *ctx, __u32 ip, __u16 port)
{
  struct rpc_server_lib *server;
  int ret, id;
  struct equeue *eq;
  struct rpc_queue_entry *qe;
  struct rpc_queue_new_server_req *ns_req;

  if (!ctx || !rpc)
  {
    LOG_ERROR("null rpc context");
    return NULL;
  }
  id = __sync_fetch_and_add(&rpc->next_serverid, 1);
  if (id >= MAX_SERVERS)
  {
    LOG_ERROR("exceeded max number of rpc servers");
    return NULL;
  }
  server = &rpc->servers[id];
  server->ctx = ctx;
  server->bind_success = -1;
  server->nservices = 0;
  server->nworkers = 0;
  server->services = NULL;
  server->id = INVALID_ID;

  eq = ctx->app_slow_q;
  qe = queue_tail(eq);
  if (!qe)
  {
    LOG_ERROR("failed to get queue tail for new server req");
    return NULL;
  }
  ns_req = &qe->data.new_server_req;
  ns_req->local_ip = ip;
  ns_req->local_port = port;
  ns_req->opaque = (__u64)server;
  ret = queue_enqueue(eq, RPC_QUEUE_NEW_SERVER_REQ);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue new server req");
    return NULL;
  }
  // poll until we get response
  while (server->bind_success == -1)
    rpc_poll_slow(ctx);
  if (!server->bind_success)
  {
    LOG_ERROR("bind failed for rpc server");
    return NULL;
  }
  // reset the flag for service registrations
  server->bind_success = -1;
  return server;
}

struct rpc_worker_lib *rpc_new_worker(struct rpc_server_lib *s, struct rpc_context_lib *ctx)
{
  struct rpc_worker_lib *worker;
  struct equeue *eq;
  struct rpc_queue_entry *qe;
  struct rpc_queue_new_worker_req *nw_req;
  int ret, index;

  if (!s || !rpc)
  {
    LOG_ERROR("null rpc context or server");
    return NULL;
  }
  // if no context provided, fall back to the server's context
  if (ctx == NULL)
    ctx = s->ctx;

  // if server ctx is also null, worker creation not possible
  if (ctx == NULL)
  {
    LOG_ERROR("failed to find rpc context for worker");
    return NULL;
  }

  index = __sync_fetch_and_add(&s->nworkers, 1);
  if (index >= MAX_WORKERS)
  {
    LOG_ERROR("exceeded max number of rpc workers per server");
    __sync_fetch_and_sub(&s->nworkers, 1);
    return NULL;
  }
  worker = &s->workers[index];
  memset(worker, 0, sizeof(struct rpc_worker_lib));
  worker->ctx = ctx;
  worker->server = s;
  worker->worker_id = INVALID_ID;
  // worker->type = RPC_ENTITY_WORKER;

  eq = ctx->app_slow_q;
  qe = queue_tail(eq);
  if (!qe)
  {
    LOG_ERROR("failed to get queue tail for new worker req");
    __sync_fetch_and_sub(&s->nworkers, 1);
    return NULL;
  }
  nw_req = &qe->data.new_worker_req;
  nw_req->server_id = s->id;
  nw_req->opaque = (__u64)worker;
  ret = queue_enqueue(eq, RPC_QUEUE_NEW_WORKER_REQ);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue new worker req");
    __sync_fetch_and_sub(&s->nworkers, 1);
    return NULL;
  }

  // poll until we get response
  while (worker->rx_buf == NULL)
    rpc_poll_slow(ctx);

  return worker;
}
// TODO: check if the service registration makes sense
int rpc_register(struct rpc_server_lib *server, __u8 service)
{
  struct equeue *eq;
  struct rpc_queue_entry *qe;
  struct rpc_queue_service_req *srv_req;

  if (!server)
  {
    LOG_ERROR("null rpc server");
    return -1;
  }

  if (!server->ctx)
  {
    LOG_ERROR("null rpc server context");
    return -1;
  }

  for (int i = 0; i < server->nservices; i++)
  {
    if (server->services[i] == service)
    {
      LOG_ERROR("service %u already registered", service);
      return -1;
    }
  }
  if (server->nservices == 0)
  {
    server->services = malloc(sizeof(__u16));
    if (!server->services)
    {
      LOG_ERROR("failed to allocate services array");
      return -1;
    }
  }
  else
  {
    __u16 *new_services = realloc(server->services,
                                  sizeof(__u16) * (server->nservices + 1));
    if (!new_services)
    {
      LOG_ERROR("failed to reallocate services array");
      return -1;
    }
    server->services = new_services;
  }

  eq = server->ctx->app_slow_q;
  qe = queue_tail(eq);
  if (!qe)
  {
    LOG_ERROR("failed to get queue tail for service req");
    return -1;
  }
  srv_req = &qe->data.service_req;
  srv_req->service_id = service;
  srv_req->opaque = (__u64)server;
  srv_req->server_id = server->id;
  int ret = queue_enqueue(eq, RPC_QUEUE_SERVICE_REQ);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue service req");
    return -1;
  }
  // poll until we get response
  while (server->bind_success == -1)
    rpc_poll_slow(server->ctx);
  if (!server->bind_success)
  {
    LOG_ERROR("service registration failed for rpc server");
    return -1;
  }
  // reset the flag for future registrations
  server->bind_success = -1;
  server->services[server->nservices] = service;
  server->nservices++;
  return 0;
}

int rpc_call(struct rpc_client_lib *c, __u32 ip, __u16 port,
             __u16 service, void *buf, size_t len)
{
  int ret, n;
  struct equeue *eq;
  struct rpc_queue_bump_entry *qe;
  struct rpc_queue_bump_cham_tx *bump;
  struct rpc_hdr hdr;
  __u32 tail, n1, n2;

  if (!c)
  {
    LOG_ERROR("null rpc client");
    return -1;
  }

  if (len == 0)
  {
    LOG_ERROR("rpc call with zero-length payload");
    return -1;
  }

  // ensure enough space in tx ring, else don't send.
  // TODO: implement fragment mechanism later on with rid's

  n = len + sizeof(struct rpc_hdr);
  if (n > c->tx_len - c->tx_avail)
    return -1;
  // n = c->tx_len - c->tx_avail;

  // rpc header is set
  hdr.service.x = htons(service);
  hdr.len.x = htons(n);
  hdr.rid.x = htonl(__sync_fetch_and_add(&rpc->next_rid, 1));
  hdr.type = 0; // request

  // fprintf(stderr, "rpc_call: service=%u client_id=%u rid=%u\n",
          // service, c->client_id, ntohl(hdr.rid.x));

  // tail pos. of tx ring
  tail = c->tx_head + c->tx_avail;
  if (tail >= c->tx_len)
    tail -= c->tx_len;

  // if (client_add_req(c, ntohl(hdr.rid.x)) != 0)
  // {
  //   LOG_ERROR("failed to track pending client request rid=%u",
  //             ntohl(hdr.rid.x));
  //   return -1;
  // }

  // copy hdr + payload to tx ring
  if (tail + sizeof(struct rpc_hdr) > c->tx_len)
  {
    n1 = c->tx_len - tail;
    n2 = sizeof(struct rpc_hdr) - n1;
    memcpy(c->tx_buf + tail, &hdr, n1);
    memcpy(c->tx_buf, ((__u8 *)&hdr) + n1, n2);
    tail = n2;
  }
  else
  {
    memcpy(c->tx_buf + tail, &hdr, sizeof(struct rpc_hdr));
    tail += sizeof(struct rpc_hdr);
  }

  if (tail + len > c->tx_len)
  {
    n1 = c->tx_len - tail;
    n2 = len - n1;
    memcpy(c->tx_buf + tail, buf, n1);
    memcpy(c->tx_buf, buf + n1, n2);
  }
  else
  {
    memcpy(c->tx_buf + tail, buf, len);
  }
  // update avail

  /* have to send the rpc_hdr + payload so that we do not have to store
  the service id and request/response information anywhere else separately */

  c->tx_avail += n;
  c->tx_port = port;
  c->tx_ip = ip;

  // enqueue bump entry to notify fast-path
  eq = c->ctx->app_fast_qs[c->core];
  qe = queue_tail(eq);
  if (!qe)
  {
    LOG_ERROR("failed to get queue tail for bump req");
    return -1;
  }
  bump = &qe->data.bump_cham_tx;
  bump->sock_id = c->client_id;
  bump->tx_ip = ip;
  bump->tx_port = port;
  bump->tx_avail = n;
  bump->type = 0; // request

  ret = queue_enqueue(eq, RPC_QUEUE_BUMP_CHAM_TX);
  if (ret != 0)
  {
    // req = client_get_req(c, ntohl(hdr.rid.x));
    // client_remove_req(req);
    LOG_ERROR("failed to enqueue bump req");
    return -1;
  }

  return (int)len;
}

int rpc_return(struct rpc_server_lib *s, struct rpc_worker_lib *w,
               __u32 rid, void *buf, size_t len)
{
  struct equeue *eq;
  struct dqueue *q;
  struct rpc_queue_bump_entry *qe;
  struct rpc_queue_bump_cham_tx *bump_tx;
  struct rpc_queue_bump_cham_rx *bump_rx;
  struct rpc_hdr hdr;
  __u32 tail, n1, n2;
  int ret, n;

  if (!s)
  {
    LOG_ERROR("null rpc server");
    return -1;
  }

  if (!w)
  {
    LOG_ERROR("null rpc worker");
    return -1;
  }

  if (len == 0)
  {
    LOG_ERROR("rpc return with zero-length payload");
    return -1;
  }

  n = len + sizeof(struct rpc_hdr);
  if (n > w->tx_len - w->tx_avail)
    return -1;

  // rpc header is set
  hdr.service.x = htons(0);
  hdr.rid.x = htonl(rid);
  hdr.len.x = htons(n);
  hdr.type = 1;

  // tail pos. of tx ring
  tail = w->tx_head + w->tx_avail;
  if (tail >= w->tx_len)
    tail -= w->tx_len;

  // copy hdr + payload to tx ring
  if (tail + sizeof(struct rpc_hdr) > w->tx_len)
  {
    n1 = w->tx_len - tail;
    n2 = sizeof(struct rpc_hdr) - n1;
    memcpy(w->tx_buf + tail, &hdr, n1);
    memcpy(w->tx_buf, ((__u8 *)&hdr) + n1, n2);
    tail = n2;
  }
  else
  {
    memcpy(w->tx_buf + tail, &hdr, sizeof(struct rpc_hdr));
    tail += sizeof(struct rpc_hdr);
  }

  if (tail + len > w->tx_len)
  {
    n1 = w->tx_len - tail;
    n2 = len - n1;
    memcpy(w->tx_buf + tail, buf, n1);
    memcpy(w->tx_buf, buf + n1, n2);
  }
  else
  {
    memcpy(w->tx_buf + tail, buf, len);
  }

  w->tx_avail += n;
  w->tx_port = w->rx_port;
  w->tx_ip = w->rx_ip;

  // enqueue TX bump to notify fast-path to send the response
  eq = w->ctx->app_fast_qs[w->server->core];
  qe = queue_tail(eq);
  if (!qe)
  {
    LOG_ERROR("failed to get queue tail for tx bump req");
    return -1;
  }
  bump_tx = &qe->data.bump_cham_tx;
  bump_tx->sock_id = w->worker_id;
  bump_tx->tx_ip = w->rx_ip;
  bump_tx->tx_port = w->rx_port;
  bump_tx->tx_avail = n;
  bump_tx->type = 1; // response

  ret = queue_enqueue(eq, RPC_QUEUE_BUMP_CHAM_TX);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue tx bump req");
    return -1;
  }

  if (!w->server->app_lb_mode)
  {
    // notify fast-path that RX space has been freed
    qe = queue_tail(eq);
    if (!qe)
    {
      LOG_ERROR("failed to get queue tail for rx bump req");
      return -1;
    }
    bump_rx = &qe->data.bump_cham_rx;
    bump_rx->sock_id = w->worker_id;
    bump_rx->rx_head = w->rx_pkt_len;
    bump_rx->type = 0; // request

    ret = queue_enqueue(eq, RPC_QUEUE_BUMP_CHAM_RX);
    if (ret != 0)
    {
      LOG_ERROR("failed to enqueue rx bump req");
      return -1;
    }

    /* Dequeue the BUMP_APP_RX that rpc_handle_call peeked at */
    q = w->ctx->fast_app_qs[w->server->core];
    queue_dequeue(q);
  }

  return len;
}

int rpc_handle_call(struct rpc_worker_lib *w, __u32 *rid,
                    void *buf, size_t len)
{
  int n;
  struct dqueue *q;
  struct rpc_queue_bump_entry *qe;
  __u32 n1, n2, new_head;
  struct rpc_hdr hdr = {0};
  struct rpc_worker *wkr = (struct rpc_worker *)w->shm_worker;
  if (!w || !rid)
  {
    LOG_ERROR("null rpc worker or rid pointer");
    return -1;
  }

  if (w->server->app_lb_mode && wkr->jobs_pending >= JOB_QUEUE_SIZE)
  {
    errno = EAGAIN;
    return -1;
  }

  if (w->rx_avail == 0)
  {
    if (w->server->app_lb_mode)
    {
      /* JBSQ pull: worker claims next entry from the server's shared ring */
      
      __u8 *shbuf;
      __u32 meta_sz, srvr_avail, entry_len, hdr_pos, dst_pos, curr_head;
      struct rpc_rx_meta md;
      struct rpc_server *srvr;

      srvr = (struct rpc_server *)w->server->shm_server;
      shbuf = (__u8 *)w->server->rx_buf;
      meta_sz    = (__u32)sizeof(struct rpc_rx_meta);
      curr_head = srvr->rx_head;
      srvr_avail = srvr->rx_avail;

      while (__sync_lock_test_and_set(&srvr->rx_lock, 1)){}

      //checks if there are enough bytes available to read otw try reading again later
      if (srvr_avail < meta_sz + (__u32)sizeof(struct rpc_hdr))
      {
        __sync_lock_release(&srvr->rx_lock);
        errno = EAGAIN;
        return -1;
      }

      if (curr_head + meta_sz <= srvr->rx_len)
      {
        memcpy(&md, shbuf + curr_head, meta_sz);
      }
      else
      {
        n1 = srvr->rx_len - curr_head;
        n2 = meta_sz - n1;
        memcpy(&md, shbuf + curr_head, n1);
        memcpy((__u8 *)&md + n1, shbuf, n2);
      }

      hdr_pos = (curr_head + meta_sz) % srvr->rx_len;
      if (hdr_pos + sizeof(struct rpc_hdr) <= srvr->rx_len)
      {
        memcpy(&hdr, shbuf + hdr_pos, sizeof(struct rpc_hdr));
      }
      else
      {
        n1 = srvr->rx_len - hdr_pos;
        n2 = (__u32)sizeof(struct rpc_hdr) - n1;
        memcpy(&hdr, shbuf + hdr_pos, n1);
        memcpy((__u8 *)&hdr + n1, shbuf, n2);
      }

      hdr.service.x = ntohs(hdr.service.x);
      hdr.len.x     = ntohs(hdr.len.x);
      hdr.rid.x     = ntohl(hdr.rid.x);

      entry_len = meta_sz + hdr.len.x;

      //checks if the whole payload is available to read
      if (srvr_avail < entry_len)
      {
        __sync_lock_release(&srvr->rx_lock);
        errno = EAGAIN;
        return -1;
      }

      if (hdr.len.x > w->rx_len - w->rx_avail)
      {
        __sync_lock_release(&srvr->rx_lock);
        LOG_ERROR("worker rx_buf too small for incoming RPC message");
        return -1;
      }

      srvr->rx_head = (curr_head + entry_len) % srvr->rx_len;
      __sync_fetch_and_sub(&srvr->rx_avail, entry_len);

      __sync_lock_release(&srvr->rx_lock);

      dst_pos = (w->rx_head + w->rx_avail) % w->rx_len;
      
      //TODO: see if this helper function has other uses
      //copy payload from the server's rx buf to worker rx_buf
      ring2ring((__u8 *)w->rx_buf, dst_pos, w->rx_len, shbuf, hdr_pos,
                srvr->rx_len, hdr.len.x);

      __sync_fetch_and_add(&wkr->jobs_pending, 1);

      w->rx_ip    = md.rx_ip;
      w->rx_port  = md.rx_port;
      w->rx_avail += hdr.len.x;
    }
    else
    {
      /* eBPF: peek at the bump queue */
      q = w->ctx->fast_app_qs[w->server->core];

      if ((qe = queue_head(q)) == NULL ||
          qe->type != RPC_QUEUE_BUMP_APP_RX ||
          qe->data.bump_app_rx.type != 0)
      {
        errno = EAGAIN;
        return -1;
      }

      /* Read address and size directly from the bump */
      w->rx_avail = qe->data.bump_app_rx.rx_avail;
      w->rx_ip    = qe->data.bump_app_rx.rx_ip;
      w->rx_port  = qe->data.bump_app_rx.rx_port;

      if (w->rx_head + sizeof(struct rpc_hdr) > w->rx_len)
      {
        n1 = w->rx_len - w->rx_head;
        n2 = sizeof(struct rpc_hdr) - n1;
        memcpy(&hdr, w->rx_buf + w->rx_head, n1);
        memcpy(((__u8 *)&hdr) + n1, w->rx_buf, n2);
      }
      else
      {
        memcpy(&hdr, w->rx_buf + w->rx_head, sizeof(struct rpc_hdr));
      }
      hdr.service.x = ntohs(hdr.service.x);
      hdr.len.x     = ntohs(hdr.len.x);
      hdr.rid.x     = ntohl(hdr.rid.x);
    }
  }

  // store metadata so the application can read it immediately after this call
  // w->last_rid = hdr.rid.x;

  // len of payload
  n = hdr.len.x - sizeof(struct rpc_hdr);

  // check if entire payload would fit in the buffer
  if (len < (size_t)n)
  {
    LOG_ERROR("buffer too small for rpc payload");
    return -1;
  }

  *rid = hdr.rid.x;
  w->rx_pkt_len = hdr.len.x;

  new_head = w->rx_head + sizeof(struct rpc_hdr);
  if (new_head >= w->rx_len)
    new_head -= w->rx_len;

  // Read payload
  if (new_head + n > w->rx_len)
  {
    n1 = w->rx_len - new_head;
    n2 = n - n1;
    memcpy(buf, w->rx_buf + new_head, n1);
    memcpy((__u8 *)buf + n1, w->rx_buf, n2);
  }
  else
  {
    memcpy(buf, w->rx_buf + new_head, n);
  }

  // advance rx head and avail 
  w->rx_avail -= hdr.len.x;
  new_head = w->rx_head + hdr.len.x;
  if (new_head >= w->rx_len)
    new_head -= w->rx_len;
  w->rx_head = new_head;

  // TODO: store some information about the call in the worker struct?

  // bump fast path
  /*
  eq = w->ctx->app_fast_qs[w->server->core];
  qe = queue_tail(eq);
  if (!qe)
  {
    LOG_ERROR("failed to get queue tail for bump req");
    return -1;
  }
  bump = &qe->data.bump_cham_rx;
  bump->sock_id = w->worker_id;
  bump->rx_head = hdr.len.x;
  bump->type = 0; // request

  ret = queue_enqueue(eq, RPC_QUEUE_BUMP_CHAM_RX);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue bump req");
    return -1;
  }*/

  return n;
}

int rpc_response(struct rpc_client_lib *c, void *buf, size_t len)
{
  int ret, n;
  struct equeue *eq;
  struct rpc_queue_bump_entry *qe;
  struct rpc_queue_bump_cham_rx *bump;
  struct rpc_hdr hdr;
  __u32 n1, n2, new_head;

  if (!c)
  {
    LOG_ERROR("null rpc client");
    return -1;
  }

  if (c->rx_avail == 0)
  {
    errno = EAGAIN;
    return -1;
  }

  // Read the header
  if (c->rx_head + sizeof(struct rpc_hdr) > c->rx_len)
  {
    n1 = c->rx_len - c->rx_head;
    n2 = sizeof(struct rpc_hdr) - n1;
    memcpy(&hdr, c->rx_buf + c->rx_head, n1);
    memcpy(((__u8 *)&hdr) + n1, c->rx_buf, n2);
  }
  else
  {
    memcpy(&hdr, c->rx_buf + c->rx_head, sizeof(struct rpc_hdr));
  }

  // Convert fields from network to host order
  hdr.service.x = ntohs(hdr.service.x);
  hdr.len.x = ntohs(hdr.len.x);
  hdr.rid.x = ntohl(hdr.rid.x);

  // payload load calculation
  n = hdr.len.x - sizeof(struct rpc_hdr);

  // Check if the entire payload fits in the buffer
  if (len < (size_t)n)
  {
    LOG_ERROR("buffer too small for rpc payload");
    return -1;
  }

  new_head = c->rx_head + sizeof(struct rpc_hdr);
  if (new_head >= c->rx_len)
    new_head -= c->rx_len;

  // Read the payload
  if (new_head + n > c->rx_len)
  {
    n1 = c->rx_len - new_head;
    n2 = n - n1;
    memcpy(buf, c->rx_buf + new_head, n1);
    memcpy((__u8 *)buf + n1, c->rx_buf, n2);
  }
  else
  {
    memcpy(buf, c->rx_buf + new_head, n);
  }

  // Update rx buffer head and avail

  /* we subtract hdr + payload instead of just payload
  because both of them were written in the RX buffer */

  c->rx_avail -= hdr.len.x;
  new_head = c->rx_head + hdr.len.x;
  if (new_head >= c->rx_len)
    new_head -= c->rx_len;
  c->rx_head = new_head;

  // bump fast path
  eq = c->ctx->app_fast_qs[c->core];
  qe = queue_tail(eq);
  if (!qe)
  {
    LOG_ERROR("failed to get queue tail for bump req");
    return -1;
  }
  bump = &qe->data.bump_cham_rx;
  bump->sock_id = c->client_id;
  bump->rx_head = hdr.len.x;
  bump->type = 1; // response

  ret = queue_enqueue(eq, RPC_QUEUE_BUMP_CHAM_RX);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue bump req");
    return -1;
  }

  return n;
}

int rpc_call_complete(struct rpc_worker_lib *w)
{
  struct rpc_worker *shm_worker;

  if (!w || w->shm_worker == NULL)
  {
    LOG_ERROR("null rpc worker in call_complete");
    return -1;
  }

  shm_worker = (struct rpc_worker *)w->shm_worker;
  if (shm_worker->jobs_pending == 0)
    return 0;

  __sync_fetch_and_sub(&shm_worker->jobs_pending, 1);
  return 0;
}

int rpc_set_app_lb(struct rpc_server_lib *server)
{
  struct rpc_server *serv;
  // int i;

  if (!server || !server->shm_server)
  {
    LOG_ERROR("null server in rpc_set_app_lb");
    return -1;
  }

  serv = (struct rpc_server *)server->shm_server;
  serv->app_lb_mode = 1;
  server->app_lb_mode = 1;

  // for (i = 0; i < server->nworkers; i++)
  //   server->workers[i].app_lb_mode = 1;

  return 0;
}

/* Copy `len` bytes from a circular ring of size `src_ring_len` starting at
   `src_pos` into a circular ring of size `dst_ring_len` starting at `dst_pos` */
static void ring2ring(__u8 *dst, __u32 dst_pos, __u32 dst_ring_len,
    const __u8 *src, __u32 src_pos, __u32 src_ring_len, __u32 len)
{
  while (len > 0)
  {
    __u32 src_chunk = src_ring_len - src_pos;
    __u32 dst_chunk = dst_ring_len - dst_pos;
    __u32 chunk = len;

    if (chunk > src_chunk) chunk = src_chunk;
    if (chunk > dst_chunk) chunk = dst_chunk;
    memcpy(dst + dst_pos, src + src_pos, chunk);
    src_pos = (src_pos + chunk) % src_ring_len;
    dst_pos = (dst_pos + chunk) % dst_ring_len;
    len -= chunk;
  }
}
/*
static int ring_read(void *dst, const __u8 *ring, __u32 pos,
                    __u32 ring_len, __u32 len)
{
    __u32 first;

    if (!dst || !ring || ring_len == 0 || pos >= ring_len)
        return -1;

    if (pos + len <= ring_len) {
        memcpy(dst, ring + pos, len);
        return 0;
    }

    first = ring_len - pos;
    memcpy(dst, ring + pos, first);
    memcpy((__u8 *)dst + first, ring, len - first);

    return 0;
}
*/
static int handle_new_client_res(struct rpc_queue_entry *qe)
{
  struct rpc_queue_new_client_res *res;
  struct rpc_client_lib *client;

  res = &qe->data.new_client_res;
  client = (struct rpc_client_lib *)res->opaque;
  client->client_id = res->client_id;
  client->core = res->core;
  client->rx_qid = res->rx_qid;
  client->rx_len = res->rx_len;
  client->rx_buf = rpc->shm_base + res->rx_off;
  client->tx_qid = res->tx_qid;
  client->tx_len = res->tx_len;
  client->tx_buf = rpc->shm_base + res->tx_off;
  client->bind_success = res->success;

  return 0;
}

static int handle_new_server_res(struct rpc_queue_entry *qe)
{
  struct rpc_queue_new_server_res *res;
  struct rpc_server_lib *server;

  res = &qe->data.new_server_res;
  server = (struct rpc_server_lib *)res->opaque;
  server->core = res->core;
  server->id = res->server_id;
  server->bind_success = res->success;
  server->shm_server = rpc->shm_base + res->server_off;
  server->rx_buf = rpc->shm_base + res->rx_off;
  server->rx_len = res->rx_len;
  server->app_lb_mode = 0;
  return 0;
}
static int handle_new_worker_res(struct rpc_queue_entry *qe)
{
  struct rpc_queue_new_worker_res *res;
  struct rpc_worker_lib *worker;

  res = &qe->data.new_worker_res;
  worker = (struct rpc_worker_lib *)res->opaque;

  worker->rx_qid = res->rx_qid;
  worker->rx_len = res->rx_len;
  worker->rx_buf = rpc->shm_base + res->rx_off;
  worker->tx_qid = res->tx_qid;
  worker->tx_len = res->tx_len;
  worker->tx_buf = rpc->shm_base + res->tx_off;
  worker->shm_worker = rpc->shm_base + res->worker_off;
  worker->worker_id = res->worker_id;
  return 0;
}

static int handle_new_service_res(struct rpc_queue_entry *qe)
{
  struct rpc_queue_service_res *res;
  struct rpc_server_lib *server;

  res = &qe->data.service_res;
  server = (struct rpc_server_lib *)res->opaque;
  server->bind_success = res->success;
  return 0;
}

static int handle_tx_bump(struct rpc_queue_bump_entry *qe)
{
  struct rpc_queue_bump_app_tx *bump;
  __u32 new_head, tx_len;
  struct rpc_client_lib *client;
  struct rpc_worker_lib *worker;
  __u32 *tx_head, *tx_avail;

  bump = &qe->data.bump_app_tx;

  switch (bump->type)
  {
  case 0: // request from client
    client = (struct rpc_client_lib *)bump->opaque;
    tx_head = &client->tx_head;
    tx_avail = &client->tx_avail;
    tx_len = client->tx_len;
    break;
  case 1: // response from server
    worker = (struct rpc_worker_lib *)bump->opaque;
    tx_head = &worker->tx_head;
    tx_avail = &worker->tx_avail;
    tx_len = worker->tx_len;
    break;
  default:
    LOG_ERROR("unknown bump type in tx bump: %u", bump->type);
    return -1;
  }

  new_head = bump->tx_head + *tx_head;
  // TODO: see if the equal has to be removed!
  if (new_head >= tx_len)
    new_head -= tx_len;
  *tx_head = new_head;
  *tx_avail -= bump->tx_head;
  return 0;
}

static int handle_rx_bump(struct rpc_queue_bump_entry *qe)
{
  struct rpc_queue_bump_app_rx *bump;
  struct rpc_client_lib *client;
  struct rpc_worker_lib *worker;

  bump = &qe->data.bump_app_rx;

  switch (bump->type)
  {
  case 1: // response delivered to client
    client = (struct rpc_client_lib *)bump->opaque;
    client->rx_avail += bump->rx_avail;
    client->rx_ip = bump->rx_ip;
    client->rx_port = bump->rx_port;
    break;
  case 0: // request delivered to worker
    worker = (struct rpc_worker_lib *)bump->opaque;
    worker->rx_avail += bump->rx_avail;
    worker->rx_ip = bump->rx_ip;
    worker->rx_port = bump->rx_port;
    break;
  default:
    LOG_ERROR("unknown bump type in rx bump: %u", bump->type);
    return -1;
  }
  return 0;
}


#include <stdlib.h>
#include <cham_lib.h>

#include "appif.h"
#include "udp_slow.h"
#include "udp.h"
#include "log.h"

int init_udp_slow_context(struct udp_slow_context *ctx);

int poll_apps(struct udp_slow_context *ctx);

int handle_new_sock(struct udp_slow_context *ctx, 
  struct udp_app_context_slow *actx, struct udp_queue_entry *qe);
int handle_bind(struct udp_slow_context *ctx, 
    struct udp_app_context_slow *actx, struct udp_queue_entry *qe_req);

int init_udp_slow_context(struct udp_slow_context *ctx)
{
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

  ctx->app_uxfd = -1;
  ctx->app_epfd = -1;
  ctx->guest = g;
  ctx->proto = p;
  ctx->n_apps = 0;
  ctx->next_app = 0;

  return 0;
}

int poll_apps(struct udp_slow_context *ctx)
{
  int msgs_i, apps_polled, ctxs_polled;
  uint8_t type;
  struct dqueue *q;
  struct udp_queue_entry *qe;
  struct udp_app_slow *a;
  struct udp_app_context_slow *actx;
 
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
    }
  
    if (apps_polled >= ctx->n_apps)
      break;
    
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
        queue_dequeue(q);
        break;
      case UDP_QUEUE_BIND:
        handle_bind(ctx, actx, qe);
        queue_dequeue(q);
        break;
      default:
        LOG_WARN("unknown queue entry type from app " 
            "to udp slow-path type=%d", type);
        break;
    }
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

  struct udp_sock *socks_map = ctx->proto->shm_base + 
      actx->app->socks_map->off;

  if (actx->app->n_socks >= MAX_SOCKETS)
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
  res->sock_id = actx->app->n_socks;
  res->core = 0;

  sock = &socks_map[res->sock_id];
  sock->id = res->sock_id;
  sock->next_id = ID_INVALID;
  sock->core = 0;
  sock->src_ip = ctx->proto->local_ip;
  sock->app_bump_qid = actx->app_bump_qs[0]->id;
  sock->opaque = req->opaque;

  /* Create queue for RX buffer */
  protoq = cham_new_queue(ctx->proto, RXBUF_SZ, 1);
  if (protoq == NULL)
  {
    LOG_ERROR("failed to create new queue");
    return -1;
  }
  res->rx_qid = protoq->id;
  res->rx_len = protoq->elsize * protoq->nelems;
  res->rx_off = protoq->off;
  sock->rx_qid = protoq->id;
  sock->rx_len = protoq->elsize * protoq->nelems;
  sock->rx_avail = 0;
  sock->rx_head = 0;
  sock->rx_off = protoq->off;

  /* Create queue for TX buffer */
  protoq = cham_new_queue(ctx->proto, TXBUF_SZ, 1);
  res->tx_qid = protoq->id;
  res->tx_len = protoq->nelems * protoq->elsize;
  res->tx_off = protoq->off;
  sock->tx_qid = protoq->id;
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

  return 0;
}

int handle_bind(struct udp_slow_context *ctx, 
    struct udp_app_context_slow *actx, struct udp_queue_entry *qe_req)
{
  struct udp_queue_entry *qe_res;
  struct udp_queue_bind *req;
  struct udp_sock *sock;

  qe_res = queue_tail(actx->slow_app_q);
  if (qe_res == NULL)
  {
    LOG_ERROR("failed to get tail of slow->app queue");
    return -1;
  }
  
  req = &qe_req->data.bind;
  struct udp_sock *socks_map = ctx->proto->shm_base + actx->app->socks_map->off;
  sock = &socks_map[req->sock_id];
  sock->src_ip = req->src_ip;
  sock->src_port = req->src_port;

  return 0;
}

int main(int argc, char **argv)
{
  int ret;
  struct udp_slow_context ctx;
  
  ret = init_udp_slow_context(&ctx);
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
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

  return 0;
}

int poll_apps(struct udp_slow_context *ctx)
{
  int i, j;
  uint8_t type;
  struct dqueue *q;
  struct udp_queue_entry *qe;

  
  /* TODO: Poll up to batch size */
  for (i = 0; i < ctx->n_apps; i++)
  {
    for (j = 0; j < ctx->apps[i].n_ctxs; j++)
    {
      q = ctx->apps[i].ctxs[j].app_slow_q;
      qe = udp_queue_head(q);

      if (qe == NULL)
        return 0;
      
      type = qe->type;
      switch (type)
      {
        case UDP_QUEUE_EMPTY:
          break;
        case UDP_QUEUE_NEW_SOCK_REQ:
          handle_new_sock(ctx, &ctx->apps[i].ctxs[j], qe);
          udp_queue_dequeue(q);
          break;
        default:
          LOG_WARN("unknown queue tryt type from app" 
              "to udp slow-path type=%d", type);
          break;
      }
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
  struct udp_sock_mape *sock;
  struct proto_queue_lib *protoq;
  struct udp_queue_entry *qe_res;
  struct udp_queue_new_sock_req *req;
  struct udp_queue_new_sock_res *res;

  struct udp_off_mape *offs_table = ctx->proto->shm_base + 
      actx->app->offs_map->off;
  struct udp_sock_mape *socks_table = ctx->proto->shm_base + 
      offs_table[MTYPE_SOCKS].off;
  
  if (offs_table[MTYPE_SOCKS].n >= offs_table[MTYPE_SOCKS].max_n)
  {
    LOG_ERROR("Socket map is full");
    return -1;
  }

  req = &qe_req->data.new_sock_req;
  qe_res = udp_queue_tail(actx->slow_app_q);
  if (qe_res == NULL)
  {
    LOG_ERROR("failed to get tail of slow->app queue");
    return -1;
  }
  res = &qe_res->data.new_sock_res;
  res->opaque = req->opaque;
  res->id_slow = offs_table[MTYPE_SOCKS].n;
  res->core = 0;

  sock = &socks_table[res->id_slow];
  sock->id = offs_table[MTYPE_SOCKS].n;
  sock->next_id = ID_INVALID;
  sock->core = 0;
  sock->app_bump_qid = actx->app_bump_qs[0]->id;
  
  if (offs_table[MTYPE_SOCKS].tail == ID_INVALID)
    offs_table[MTYPE_SOCKS].head = sock->id;
  else
    socks_table[offs_table[MTYPE_SOCKS].tail].next_id = sock->id;

  offs_table[MTYPE_SOCKS].tail = sock->id;
  offs_table[MTYPE_SOCKS].n++;

  /* Create queue for RX buffer */
  /* TODO: Have size of buffer be a parameter */
  protoq = cham_new_queue(ctx->proto, 16384);
  if (protoq == NULL)
  {
    LOG_ERROR("failed to create new queue");
    return -1;
  }
  res->rx_qid = protoq->id;
  res->rx_len = protoq->size;
  res->rx_off = protoq->off;
  sock->rx_qid = protoq->id;
  sock->rx_len = protoq->size;
  sock->rx_avail = 0;
  sock->rx_head = 0;
  sock->rx_buf = ctx->proto->shm_base + protoq->off;

  /* Create queue for TX buffer */
  /* TODO: Have size of buffer be a parameter */
  protoq = cham_new_queue(ctx->proto, 16384);
  res->tx_qid = protoq->id;
  res->tx_len = protoq->size;
  res->tx_off = protoq->off;
  sock->tx_qid = protoq->id;
  sock->tx_len = protoq->size;
  sock->tx_avail = 0;
  sock->tx_head = 0;
  sock->tx_buf = ctx->proto->shm_base + protoq->off;

  /* Send response to app */
  ret = udp_queue_enqueue(actx->slow_app_q, UDP_QUEUE_NEW_SOCK_RES);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue new socket response");
    return -1;
  }

  return 0;
}
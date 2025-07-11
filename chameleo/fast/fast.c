#include <stdlib.h>

#include <rte_malloc.h>

#include "network.h"
#include "fast.h"
#include "fast_process.h"
#include "../protocols/udp/udp.h"
#include "../../utils/log/log.h"
#include "../config/config.h"

int poll_rx(struct fast_path_context *ctx);
int poll_queues(struct fast_path_context *ctx);
int poll_tx(struct fast_path_context *ctx);

int fast_path_context_init(struct fast_path_context *fp_ctx, 
    struct rte_eth_dev_info *eth_dev_info, 
    struct configuration *config, uint16_t thread_id, uint8_t port_id)
{

  fp_ctx->id = thread_id;
  network_init(&fp_ctx->net_ctx, 
      eth_dev_info, config, thread_id, port_id);
  
  /* TODO: Add this dynamically when VMs and applications start */
  struct guest *g = rte_zmalloc("guest", sizeof(struct guest), 0);
  g->id = 0;
  g->next_guest = NULL;
  g->n_apps = 1;
  struct application *app = rte_zmalloc("app", sizeof(struct application), 0);
  g->apps = app;
  app->id = 0;
  app->proto.id = PROTOCOL_UDP;
  app->proto.process_rx = udp_process_rx;
  app->proto.process_queues = udp_process_queues;
  app->proto.process_tx = udp_process_tx;
    
  return 0;
}

void fast_path_context_destroy()
{
  /* TODO: cleanup fast path context */
}

int fast_path_loop(struct fast_path_context *ctx)
{
  int ret;

  while(1) 
  {
    ret = poll_rx(ctx);
    if (ret < 0)
    {
      LOG_ERROR("poll_rx failed");
      return -1;
    }

    ret = poll_queues(ctx);
    if (ret < 0)
    {
      LOG_ERROR("poll_queues failed");
    }

    ret = poll_tx(ctx);
    if (ret < 0)
    {
      LOG_ERROR("poll_tx failed");
    }
  }
}

int poll_rx(struct fast_path_context *ctx)
{
  int i, n, ret;
  struct rte_mbuf *mbs[BATCH_SIZE];

  n = BATCH_SIZE;

  /* receive packets from the network */
  ret = network_rx(&ctx->net_ctx, n, mbs);

  if (ret <= 0)
  {
    return 0;
  }

  for (i = 0; i < n; i++)
  {
    ret = fast_process_packet_rx(ctx, mbs[i]);
    if (ret < 0)
    {
      LOG_ERROR("fast_process_packet_rx failed");
      return -1;
    }
  }

  return n;
}

int poll_queues(struct fast_path_context *ctx)
{
  return 0;
}

int poll_tx(struct fast_path_context *ctx)
{
  int i, ret;
  struct guest *guest;
  struct rte_mbuf *mbs[BATCH_SIZE];
  uint8_t n_guests = ctx->n_guests;
  
  /* Allocate mbufs to use for transmission */
  ret = rte_pktmbuf_alloc_bulk(ctx->net_ctx.pool, mbs, BATCH_SIZE);
  if (ret < 0)
  {
    LOG_ERROR("not enough entries in the mempool");
    abort();
    return -1;
  }

  guest = ctx->guests;
  for (i = 0; i < n_guests && guest != NULL; i++)
  {
    ret = fast_process_packet_tx(ctx, mbs[i]);
    if (ret < 0)
    {
      LOG_ERROR("fast_process_packet_tx failed");
      return -1;
    }
    guest = guest->next_guest;
  }

  ret = network_tx(&ctx->net_ctx, ctx->tx_n, ctx->tx_mbs);
  rte_pktmbuf_free_bulk(mbs, BATCH_SIZE);

  if (ret == ctx->tx_n)
  {
    /* Everything sent */
    ctx->tx_n = 0;
  }
  else if (ret > 0)
  {
    /* Move unsent packets to front */
    LOG_DEBUG("Not everything was sent for some reason");
    for (i = ret; i < ctx->tx_n; i++)
    {
      ctx->tx_mbs[i - ret] = ctx->tx_mbs[i];
    }
    ctx->tx_n -= ret;
  }

  return 0;
}
#include <stdlib.h>
#include <sys/mman.h>

#include <rte_malloc.h>

#include "network.h"
#include "fast.h"
#include "fast_process.h"
#include "../queue/queue.h"
#include "../protocols/udp/udp.h"
#include "../protocols/udp/udp_shm.h"
#include "../../utils/log/log.h"
#include "../config/config.h"

struct guest * init_guest(uint8_t id, uint64_t shm_len);
struct application * init_application(uint8_t id);

int poll_rx(struct fast_context *ctx);
int poll_queues(struct fast_context *ctx);
int poll_tx(struct fast_context *ctx);
int poll_slow(struct fast_context *ctx);

int fast_context_init(struct fast_context *f_ctx, 
    struct rte_eth_dev_info *eth_dev_info, 
    struct queue *fast_slow_q, struct queue *slow_fast_q,
    struct configuration *config, uint16_t thread_id, uint8_t port_id)
{

  f_ctx->id = thread_id;
  network_init(&f_ctx->net_ctx, 
      eth_dev_info, config, thread_id, port_id);
  
  f_ctx->fast_slow_q = fast_slow_q;
  f_ctx->slow_fast_q = slow_fast_q;

  /* TODO: Add this dynamically when VMs and applications start */
  struct guest *g = init_guest(0, config->shm_len);
  f_ctx->guests = g;
  f_ctx->n_guests = 1;

  return 0;
}

void fast_context_destroy()
{
  /* TODO: cleanup fast path context */
}

int fast_loop(struct fast_context *ctx)
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

    ret = poll_slow(ctx);
    if (ret < 0)
    {
      LOG_ERROR("poll_slow failed");
    }
  }
}

int poll_rx(struct fast_context *ctx)
{
  int i, n;
  uint8_t rx_err;
  struct rte_mbuf *mbs[BATCH_SIZE];

  n = BATCH_SIZE;
  if (TXBUF_SIZE - ctx->tx_n < n)
    n = TXBUF_SIZE - ctx->tx_n;

  /* Receive packets from the NIC */
  n = network_rx(&ctx->net_ctx, n, mbs);

  if (n <= 0)
  {
    return 0;
  }

  for (i = 0; i < n; i++)
  {
    rx_err = fast_process_packet_rx(ctx, mbs[i]);
    if (rx_err != 0)
    {
      LOG_ERROR("fast_process_packet_rx failed");
      fast_process_packet_error(ctx, mbs[i], rx_err);
    }
  }

  rte_pktmbuf_free_bulk(mbs, n);

  return n;
}

int poll_queues(struct fast_context *ctx)
{
  return 0;
}

int poll_tx(struct fast_context *ctx)
{
  unsigned n;
  int i, ret;
  uint8_t tx_err;
  struct guest *guest;
  struct rte_mbuf *mbs[BATCH_SIZE];
  uint8_t n_guests = ctx->n_guests;
  
  n = BATCH_SIZE;
  if (TXBUF_SIZE - ctx->tx_n < n)
    n = TXBUF_SIZE - ctx->tx_n;

  /* Allocate mbufs to use for transmission */
  /* TODO: Have a cache for the mempool, free only what we used and don't allocate every loop */
  ret = rte_pktmbuf_alloc_bulk(ctx->net_ctx.pool, mbs, n);
  if (ret < 0)
  {
    LOG_ERROR("not enough entries in the mempool");
    abort();
    return -1;
  }

  guest = ctx->guests;
  for (i = 0; i < n_guests && guest != NULL && i < n; i++)
  {
    tx_err = fast_process_packet_tx(ctx, mbs[i]);
    if (tx_err < 0)
    {
      LOG_WARN("fast_process_packet_tx failed sending to slow path");
      fast_process_packet_error(ctx, mbs[i], tx_err);
    }
    else
    {
      ctx->tx_mbs[ctx->tx_n] = mbs[i];
      ctx->tx_n++;
    }
    guest = guest->next_guest;
  }

  /* Push packets to the NIC */
  ret = network_tx(&ctx->net_ctx, ctx->tx_n, ctx->tx_mbs);
  rte_pktmbuf_free_bulk(mbs, n);

  if (ret == ctx->tx_n)
  {
    /* Everything sent */
    ctx->tx_n = 0;
  }
  else if (ret > 0)
  {
    /* Move unsent packets to front */
    for (i = ret; i < ctx->tx_n; i++)
    {
      ctx->tx_mbs[i - ret] = ctx->tx_mbs[i];
    }
    ctx->tx_n -= ret;
  }

  return 0;
}

int poll_slow(struct fast_context *ctx)
{
  uint8_t type;
  struct queue *q;
  struct queue_entry *qe;

  q = ctx->slow_fast_q;
  qe = queue_head(q);
  
  if (qe != NULL)
  {
    type = qe->type;
    switch (type)
    {
    case QUEUE_TYPE_ARP_TX:
      queue_dequeue(q);
      LOG_DEBUG("received arp tx from slow");
      break;
    default:
      LOG_WARN("unknown queue tryt type from slow path to fast path");
      break;
    }
  }

  return 0;
}

struct guest * init_guest(uint8_t id, uint64_t shm_len)
{
  uint64_t shm_internal_len;
  void *shm_base, *shm_base_internal;
  char shm_name[30], shm_internal_name[30];
  struct application *app;
  struct guest *g;

  g = rte_zmalloc("guest", sizeof(struct guest), 0);
  if (g == NULL)
  {
    LOG_ERROR("failed to allocate guest");
    goto guest_init_error;
  }
  g->id = id;
  g->next_guest = NULL;

  snprintf(shm_name, sizeof(shm_name), "%s_%d", CHAMELIO_SHM_NAME, id);
  /* TODO: Dynamically use shm init protocol according to the protocol registered by VM */
  shm_base = udp_init_shm(id, shm_name, shm_len);
  if (shm_base == NULL)
  {
    LOG_ERROR("failed to initialise shared memory for guest %d", id);
    goto shm_init_error;
  }
  g->shm_base = shm_base;
  g->shm_internal_len = shm_len;

  snprintf(shm_internal_name, sizeof(shm_internal_name), 
      "%s_%d", CHAMELIO_SHM_INTERNAL, id);
  shm_internal_len = sizeof(struct udp_shm_internal);
  /* TODO: Dynamically use shm init internal protocol according to the protocol registered by VM */
  shm_base_internal = udp_init_shm_internal(id, shm_internal_name);
  if (shm_base == NULL)
  {
    LOG_ERROR("failed to initialise internal shared memory for guest %d", id);
    goto shm_init_internal_error;
  }
  g->shm_internal_base = shm_base_internal;
  g->shm_internal_len = shm_internal_len;
  
  app = init_application(0);
  if (app == NULL)
  {
    LOG_ERROR("failed to allocate application");
    goto app_init_error;
  }
  g->n_apps = 1;

  return g;

app_init_error:
  shm_destroy_huge(shm_internal_name, 
      sizeof(struct udp_shm_internal), shm_internal_name);
shm_init_internal_error:
  shm_destroy_huge(shm_name, shm_len, shm_base);
shm_init_error:
  rte_free(g);
guest_init_error:
  return NULL;
}

struct application * init_application(uint8_t id)
{
  struct application *app;
  
  app = rte_zmalloc("app", sizeof(struct application), 0);
  if (app == NULL)
  {
    LOG_ERROR("Failed to initialise applicaiton");
    return NULL;
  }

  app->id = id;
  app->proto.id = PROTOCOL_UDP;
  app->proto.process_rx = udp_process_rx;
  app->proto.process_queues = udp_process_queues;
  app->proto.process_tx = udp_process_tx;

  return app;
}
#include <stdlib.h>
#include <linux/types.h>

#include <rte_mbuf.h>
#include <rte_malloc.h>

#include "nic_fast.h"
#include "nic.h"
#include "log.h"

#define BUFFER_SIZE 2048
#define PERTHREAD_MBUFS 2048
#define MBUF_SIZE (BUFFER_SIZE + sizeof(struct rte_mbuf) + RTE_PKTMBUF_HEADROOM)
#define RX_DESCRIPTORS 256
#define TX_DESCRIPTORS 128

static rte_spinlock_t initlock = RTE_SPINLOCK_INITIALIZER;
static volatile __u32 tx_init_done = 0;
static volatile __u32 rx_init_done = 0;
static volatile __u32 start_done = 0;

static struct rte_mempool *mempool_alloc(__u16 pool_id);
static int reta_setup(__u8 port_id, 
    __u16 fp_cores_max, __u16 reta_size);

int nic_fast_init(struct nic_context *nic_ctx,
    struct nic_fast_context *nic_fast_ctx, __u16 queue_id,
    struct configuration *config)
{
  int ret;
  unsigned int rx_socket_id, tx_socket_id;
  
  nic_fast_ctx->port_id = nic_ctx->port_id;
  nic_fast_ctx->queue_id = queue_id;

  /* Allocate memory pool that will hold packets */
  if ((nic_fast_ctx->pool = mempool_alloc(queue_id)) == NULL) 
  {
    goto error_mempool;
  }

  /* Initialize TX queue */
  rte_spinlock_lock(&initlock);
  tx_socket_id = rte_socket_id();
  ret = rte_eth_tx_queue_setup(nic_ctx->port_id, nic_fast_ctx->queue_id, 
      TX_DESCRIPTORS, tx_socket_id, 
      &nic_ctx->eth_dev_info.default_txconf);
  rte_spinlock_unlock(&initlock);

  if (ret != 0) 
  {
    LOG_ERROR("rte_eth_tx_queue_setup failed");
    goto error_tx_queue;
  }

  /* Barrier to make sure TX queues are initialized first */
  __sync_add_and_fetch(&tx_init_done, 1);
  while (tx_init_done < config->fp_cores_max);

  /* Initialize RX queue */
  rte_spinlock_lock(&initlock);
  rx_socket_id = rte_socket_id();
  ret = rte_eth_rx_queue_setup(nic_ctx->port_id, nic_fast_ctx->queue_id, 
      RX_DESCRIPTORS, rx_socket_id, 
      &nic_ctx->eth_dev_info.default_rxconf, nic_fast_ctx->pool);
  rte_spinlock_unlock(&initlock);

  if (ret != 0) 
  {
    LOG_ERROR("rte_eth_rx_queue_setup failed");
    goto error_rx_queue;
  }

  /* Barrier to make sure RX queues are initialized first */
  __sync_add_and_fetch(&rx_init_done, 1);
  while (rx_init_done < config->fp_cores_max);

  /* Start device if this is core 0 */
  if (queue_id == 0) 
  {
    if (rte_eth_dev_start(nic_ctx->port_id) != 0) 
    {
      LOG_ERROR("rte_eth_dev_start failed");
      goto error_tx_queue;
    }

    /* Setting up RETA failed */
    if (reta_setup(nic_ctx->port_id, config->fp_cores_max, 
        nic_ctx->eth_dev_info.reta_size) != 0) 
    {
      LOG_ERROR("RETA setup failed\n");
      goto error_tx_queue;
    }
    
    start_done = 1;
  }

  /* Barrier wait for main thread to start the dvice */
  while (!start_done);

  return 0;

error_rx_queue:
error_tx_queue:
  rte_mempool_free(nic_fast_ctx->pool);
error_mempool:
  free(nic_ctx);
  return -1;
}

static struct rte_mempool *mempool_alloc(__u16 pool_id)
{
  unsigned int socket_id;
  char name[32];

  snprintf(name, 32, "mbuf_pool_%u\n", pool_id);

  socket_id = rte_socket_id();

  return rte_mempool_create(name, PERTHREAD_MBUFS, MBUF_SIZE, 32,
        sizeof(struct rte_pktmbuf_pool_private), rte_pktmbuf_pool_init, NULL,
        rte_pktmbuf_init, NULL, socket_id, 0);
}

static int reta_setup(__u8 port_id, 
    __u16 fp_cores_max, __u16 reta_size)
{
  __u16 i, c;
  static struct rte_eth_rss_reta_entry64 *rss_reta;

  /* Allocate RSS redirection table and core-bucket count table */
  if (reta_size == 0) 
  {
    LOG_WARN("NIC does not expose reta size\n");
    reta_size = rte_align32pow2(fp_cores_max);
  }

  if (!rte_is_power_of_2(reta_size)) 
  {
    LOG_ERROR("reta size (%u) is not a power of 2\n", reta_size);
    return -1;
  }

  rss_reta = rte_calloc("rss reta", ((reta_size + RTE_ETH_RETA_GROUP_SIZE - 1) /
        RTE_ETH_RETA_GROUP_SIZE), sizeof(*rss_reta), 0);

  if (rss_reta == NULL) 
  {
    LOG_ERROR("reta_setup: rss_reta alloc failed\n");
    goto error_exit;
  }

  /* initialize reta */
  for (i = 0, c = 0; i < reta_size; i++) 
  {
    rss_reta[i / RTE_ETH_RETA_GROUP_SIZE].mask = -1ULL;
    rss_reta[i / RTE_ETH_RETA_GROUP_SIZE].reta[i % RTE_ETH_RETA_GROUP_SIZE] = c;
    c = (c + 1) % fp_cores_max;
  }

  if (rte_eth_dev_rss_reta_update(port_id, rss_reta, reta_size)) 
  {
    LOG_ERROR("rte_eth_dev_rss_reta_update failed\n");
    goto error_exit;
  }

  return 0;

error_exit:
  rte_free(rss_reta);
  return -1;
}
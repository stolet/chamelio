#include <stdlib.h>

#include "network.h"
#include "fast.h"
#include "fast_process.h"
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
  
  return 0;
}

void fast_path_loop(struct fast_path_context *ctx)
{
  int n;

  while(1) 
  {
    n = 0; 
    
    n = poll_rx(ctx);
    n = poll_queues(ctx);
    n = poll_tx(ctx);
  }
}

void fast_path_context_destroy()
{

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
    fast_process_packet_rx(ctx, mbs[i]);
  }

  return n;
}

int poll_queues(struct fast_path_context *ctx)
{
  return 0;
}

int poll_tx(struct fast_path_context *ctx)
{
  return 0;
}
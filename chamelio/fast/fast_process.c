#include <rte_mbuf.h>

#include "fast.h"
#include "eth.h"
#include "ip.h"
#include "udp.h"

uint8_t fast_process_packet_rx(struct fast_context *ctx, struct rte_mbuf *mb)
{
  uint8_t ret;
  
  ret = eth_process_rx(NULL, mb);
  if (ret < 0)
  {
    LOG_ERROR("failed to process eth");
    return -1;
  }
  
  ret = ip_process_rx(NULL, mb);
  if (ret < 0)
  {
    LOG_ERROR("failed to process ip");
    return -1;
  }

  ret = udp_process_rx(NULL, mb);
  if (ret < 0)
  {
    LOG_ERROR("failed to process udp");
  }

  return 0;
}

uint8_t fast_process_packet_tx(struct fast_context *ctx, struct rte_mbuf *mb)
{
  uint8_t ret;
  
  mb->data_off = 0;

  /* TODO: Don't hardcode udp here and instead use protocol assigned to application */
  ret = udp_process_tx(NULL, mb);
  if (ret < 0)
  {
    LOG_ERROR("failed to process udp");
    return ret;
  }
  
  ret = ip_process_tx(NULL, mb);
  if (ret < 0)
  {
    LOG_ERROR("failed to process ip");
    return ret;
  }
  
  ret = eth_process_tx(NULL, mb);
  if (ret < 0)
  {
    LOG_ERROR("failed to process eth");
    return ret;
  }

  mb->pkt_len = mb->data_len = 1500;
  return 0;
}
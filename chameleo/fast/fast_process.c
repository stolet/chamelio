#include <rte_mbuf.h>

#include "fast.h"
#include "protocols/eth/eth.h"
#include "protocols/ip/ip.h"
#include "protocols/udp/udp.h"

int fast_process_packet_rx(struct fast_path_context *ctx, struct rte_mbuf *mb)
{
  int ret;
  
  ret = eth_process_rx(mb);
  if (ret < 0)
  {
    LOG_ERROR("failed to process eth");
    return -1;
  }
  
  ret = ip_process_rx(mb);
  if (ret < 0)
  {
    LOG_ERROR("failed to process ip");
    return -1;
  }

  ret = udp_process_rx(mb);
  if (ret < 0)
  {
    LOG_ERROR("failed to process udp");
  }

  return 0;
}

int fast_process_packet_tx(struct fast_path_context *ctx, struct rte_mbuf *mb)
{
  int ret;
  
  /* TODO: Don't hardcode udp here and instead use protocol assigned to application */
  ret = udp_process_tx(mb);
  if (ret < 0)
  {
    LOG_ERROR("failed to process udp");
  }
  
  ret = ip_process_tx(mb);
  if (ret < 0)
  {
    LOG_ERROR("failed to process ip");
    return -1;
  }
  
  ret = eth_process_tx(mb);
  if (ret < 0)
  {
    LOG_ERROR("failed to process eth");
    return -1;
  }

  mb->data_off = 0;
  mb->pkt_len = mb->data_len = 1500;
  return 0;
}
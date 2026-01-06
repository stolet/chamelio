#include "nic.h"
#include "config.h"
#include "log.h"

/* TODO: Add flow rule to distribute gre packets with RSS */
int nic_init(struct nic_context *nic_ctx, struct configuration *config)
{
  int ret;
  __u16 n_ports, p;
  
  __u8 port_id = 0;
  struct rte_eth_dev_info *dev_info;
  
  n_ports = rte_eth_dev_count_avail();
  if (n_ports == 0)
  {
    LOG_ERROR("No ethernet devices");
    return -1;
  }
  else if (n_ports > 1)
  {
    LOG_ERROR("Multiple ethernet devices");
    // return -1;
  }

  memset(&nic_ctx->port_conf, 0, sizeof(nic_ctx->port_conf));
  struct rte_eth_conf *port_conf = &nic_ctx->port_conf;

  /* Setup receive configuration */
  port_conf->rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;
  port_conf->rxmode.offloads = 0;
  port_conf->rx_adv_conf.rss_conf.rss_hf = RTE_ETH_RSS_NONFRAG_IPV4_UDP;

  /* Setup transmit configuration */
  port_conf->txmode.mq_mode = RTE_ETH_MQ_RX_RSS;
  port_conf->txmode.offloads = 0;

  /* Setup interrupt configuration */
  port_conf->intr_conf.rxq = 1;

  /* Get port id */
  RTE_ETH_FOREACH_DEV(p) {
    port_id = p;
  }
  nic_ctx->port_id = port_id;

  /* Get MAC address and device info */
  rte_eth_macaddr_get(port_id, &nic_ctx->eth_addr);
  ret = rte_eth_dev_info_get(port_id, &nic_ctx->eth_dev_info);
  if (ret < 0)
  {
    LOG_ERROR("Failed to get device info");
    return -1;
  }

  dev_info = &nic_ctx->eth_dev_info;
  if (dev_info->max_tx_queues < config->fp_cores_max ||
      dev_info->max_rx_queues < config->fp_cores_max)
  {
    LOG_ERROR("NIC does not support enough HW queues (rx=%u tx=%u) "
        "for the requested number of cores (%u)",
        dev_info->max_rx_queues,
        dev_info->max_tx_queues,
        config->fp_cores_max);
    return -1;
  }

  /* Mask unsupported RSS hash functions */
  if ((port_conf->rx_adv_conf.rss_conf.rss_hf &
      dev_info->flow_type_rss_offloads) !=
      port_conf->rx_adv_conf.rss_conf.rss_hf)
  {
    LOG_WARN("NIC does not support all requested RSS "
        "hash functions");
    port_conf->rx_adv_conf.rss_conf.rss_hf &= dev_info->flow_type_rss_offloads;
  }

  // port_conf->txmode.offloads = RTE_ETH_TX_OFFLOAD_IPV4_CKSUM;
  // port_conf->txmode.offloads |= RTE_ETH_TX_OFFLOAD_TCP_CKSUM;
  // port_conf->txmode.offloads |= RTE_ETH_TX_OFFLOAD_UDP_CKSUM;

  /* Initialize port */
  ret = rte_eth_dev_configure(port_id, config->fp_cores_max, 
      config->fp_cores_max, port_conf);
  if (ret < 0)
  {
    LOG_ERROR("rte_eth_dev_configure failed");
    return -1;
  }
  
  /* Set offloads to dev info*/
  dev_info->default_rxconf.offloads = 0;
  dev_info->default_txconf.offloads = 0;
  // dev_info->default_txconf.offloads = RTE_ETH_TX_OFFLOAD_IPV4_CKSUM;
  // dev_info->default_txconf.offloads |= RTE_ETH_TX_OFFLOAD_TCP_CKSUM;
  // dev_info->default_txconf.offloads |= RTE_ETH_TX_OFFLOAD_UDP_CKSUM;

  return 0;
}

void nic_cleanup(struct nic_context *nic_ctx)
{
  struct rte_flow_error error;

  rte_flow_flush(nic_ctx->port_id, &error);
  rte_eth_dev_stop(nic_ctx->port_id);
}
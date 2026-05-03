#include "nic.h"

#include <string.h>

#include <rte_flow.h>
#include <rte_malloc.h>

#include "config.h"
#include "log.h"

static int gre_rss_flow_setup(__u8 port_id, const __u16 *queues,
    __u32 nr_queues, enum rte_flow_item_type l4_type, __u64 rss_types);
static void flow_err_log(const char *op, const struct rte_flow_error *err);

int nic_init(struct nic_context *nic_ctx, struct configuration *config)
{
  int ret;
  __u16 n_ports, p;
  __u64 requested_tx_offloads;
  
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
  port_conf->rx_adv_conf.rss_conf.rss_hf =
      RTE_ETH_RSS_NONFRAG_IPV4_TCP |
      RTE_ETH_RSS_NONFRAG_IPV4_UDP;

  /* Setup transmit configuration */
  port_conf->txmode.mq_mode = RTE_ETH_MQ_TX_NONE;
  port_conf->txmode.offloads = 0;

  /* Setup interrupt configuration */
  /* Datapath is poll-mode, so keep RX queue interrupts disabled. */
  port_conf->intr_conf.rxq = 0;

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

  requested_tx_offloads = RTE_ETH_TX_OFFLOAD_IPV4_CKSUM |
      RTE_ETH_TX_OFFLOAD_UDP_CKSUM;
  port_conf->txmode.offloads = requested_tx_offloads &
      dev_info->tx_offload_capa;

  if (port_conf->txmode.offloads != requested_tx_offloads)
  {
    LOG_WARN("NIC doesn't support all requested TX checksum offloads "
        "(requested=0x%llx supported=0x%llx enabled=0x%llx)",
        (unsigned long long) requested_tx_offloads,
        (unsigned long long) dev_info->tx_offload_capa,
        (unsigned long long) port_conf->txmode.offloads);
  }

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
  dev_info->default_txconf.offloads = port_conf->txmode.offloads &
      dev_info->tx_queue_offload_capa;

  return 0;
}

int nic_gre_rss_setup(struct nic_context *nic_ctx, struct configuration *config)
{
  __u16 *queues;
  __u32 i;
  int ret;

  queues = rte_calloc("gre rss queues", config->fp_cores_max, sizeof(*queues),
      0);
  if (queues == NULL)
  {
    LOG_ERROR("failed to allocate GRE RSS queues");
    return -1;
  }

  for (i = 0; i < config->fp_cores_max; i++)
    queues[i] = i;

  ret = gre_rss_flow_setup(nic_ctx->port_id, queues, config->fp_cores_max,
      RTE_FLOW_ITEM_TYPE_TCP, RTE_ETH_RSS_NONFRAG_IPV4_TCP);
  if (ret != 0)
    goto error_exit;

  ret = gre_rss_flow_setup(nic_ctx->port_id, queues, config->fp_cores_max,
      RTE_FLOW_ITEM_TYPE_UDP, RTE_ETH_RSS_NONFRAG_IPV4_UDP);
  if (ret != 0)
    goto error_exit;

  rte_free(queues);
  return 0;

error_exit:
  rte_flow_flush(nic_ctx->port_id, NULL);
  rte_free(queues);
  return -1;
}

void nic_cleanup(struct nic_context *nic_ctx)
{
  struct rte_flow_error error;

  rte_flow_flush(nic_ctx->port_id, &error);
  rte_eth_dev_stop(nic_ctx->port_id);
}

static int gre_rss_flow_setup(__u8 port_id, const __u16 *queues,
    __u32 nr_queues, enum rte_flow_item_type l4_type, __u64 rss_types)
{
  struct rte_flow_attr attr;
  struct rte_flow_action_rss rss;
  struct rte_flow_item pattern[] = {
    { .type = RTE_FLOW_ITEM_TYPE_ETH },
    { .type = RTE_FLOW_ITEM_TYPE_IPV4 },
    { .type = RTE_FLOW_ITEM_TYPE_GRE },
    { .type = RTE_FLOW_ITEM_TYPE_IPV4 },
    { .type = l4_type },
    { .type = RTE_FLOW_ITEM_TYPE_END },
  };
  struct rte_flow_action actions[] = {
    { .type = RTE_FLOW_ACTION_TYPE_RSS, .conf = &rss },
    { .type = RTE_FLOW_ACTION_TYPE_END },
  };
  struct rte_flow_error err;
  struct rte_flow *flow;
  int ret;

  memset(&attr, 0, sizeof(attr));
  attr.ingress = 1;

  memset(&rss, 0, sizeof(rss));
  rss.level = 2;
  rss.types = rss_types;
  rss.queue_num = nr_queues;
  rss.queue = queues;

  ret = rte_flow_validate(port_id, &attr, pattern, actions, &err);
  if (ret != 0)
  {
    flow_err_log("validate GRE RSS flow", &err);
    return -1;
  }

  flow = rte_flow_create(port_id, &attr, pattern, actions, &err);
  if (flow == NULL)
  {
    flow_err_log("create GRE RSS flow", &err);
    return -1;
  }

  return 0;
}

static void flow_err_log(const char *op, const struct rte_flow_error *err)
{
  if (err != NULL && err->message != NULL)
    LOG_ERROR("%s failed: %s", op, err->message);
  else
    LOG_ERROR("%s failed", op);
}

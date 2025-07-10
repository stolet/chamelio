#include <rte_malloc.h>

#include "chameleo.h"
#include "config/config.h"
#include "fast/fast.h"
#include "slow/slow.h"
#include "nic/nic.h"
#include "../utils/log/log.h"

struct chameleo_context chameleo_ctx;

int main (int argc, char **argv)
{
  int ret;
  struct configuration *config;


  /* Parse command line options */
  config = &chameleo_ctx.config;
  ret = config_parse(config, argc, argv);
  if (ret != 0)
  {
    LOG_ERROR("config_parse failed");
    goto error_exit;
  }

  /* Initialize DPDK */
  ret = rte_eal_init(config->dpdk_argc, config->dpdk_argv);
  if (ret < 0)
  {
    LOG_ERROR("DPDK init failed");
    goto error_exit;
  }

  /* Initialize NIC */
  ret = nic_init();
  if (ret != 0)
  {
    LOG_ERROR("failed to initialize nic");
    goto error_nic;
  }

  /* Initialize fast-paths */
  fast_path_init();

  /* Initialize slow-path */
  slow_path_init();

error_nic:
  nic_cleanup(&chameleo_ctx.nic_ctx);
error_exit:
  return -1;
}

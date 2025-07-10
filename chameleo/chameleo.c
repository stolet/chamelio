#define _GNU_SOURCE
#include <stdint.h>
#include <pthread.h>

#include <rte_malloc.h>

#include "chameleo.h"
#include "config/config.h"
#include "fast/fast.h"
#include "slow/slow.h"
#include "nic/nic.h"
#include "../utils/log/log.h"

struct chameleo_context chameleo_ctx;

static int fast_path_thread(void *arg);
static int fast_path_start(struct configuration *config);

int main (int argc, char **argv)
{
  int ret;
  struct configuration *config;
  struct fast_path_context **fp_ctxs;
  unsigned threads_launched;


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

  /* Initialize fast path contexts in hugepages */
  fp_ctxs = rte_calloc("fast path context list", 
      chameleo_ctx.config.fp_cores_max, sizeof(*fp_ctxs), 64);
  if (fp_ctxs == NULL)
  {
    LOG_ERROR("failed to allocated fp_ctxs");
    goto error_nic;
  }
  chameleo_ctx.fp_ctxs = fp_ctxs;

  /* Start fast-path threads */
  threads_launched = fast_path_start(&chameleo_ctx.config);
  if (threads_launched < chameleo_ctx.config.fp_cores_max)
  {
    LOG_ERROR("failed to initialize fast path launched=%d target=%d", 
        threads_launched, chameleo_ctx.config.fp_cores_max);
    goto error_nic;
  }

  /* Initialize slow-path */
  slow_path_init();
  slow_path_loop();

error_nic:
  nic_cleanup(&chameleo_ctx.nic_ctx);
error_exit:
  return -1;
}

int fast_path_start(struct configuration *config)
{
  unsigned cores_avail, cores_needed, core, threads_launched = 0;
  void *arg;

  uint32_t fp_cores_max = config->fp_cores_max;

  /* fast path cores + one slow path core */
  cores_needed = fp_cores_max + 1;
  cores_avail = rte_lcore_count();

  /* check that we have enough cores */
  if (cores_avail < cores_needed) 
  {
    LOG_ERROR("Not enough cores: got %u need %u\n", 
        cores_avail, cores_needed);
    return -1;
  }

  /* start common threads */
  RTE_LCORE_FOREACH_WORKER(core) 
  {
    if (threads_launched < fp_cores_max) 
    {
      arg = (void *) (uintptr_t) threads_launched;
    
      if (rte_eal_remote_launch(fast_path_thread, arg, core) != 0) 
      {
        LOG_ERROR("failed to launch fast path thread");
        return -1;
      }

      threads_launched++;
    }
  }

  return threads_launched;
}

static int fast_path_thread(void *arg)
{
  int ret;
  uint16_t id = (uintptr_t) arg;
  struct fast_path_context *ctx;

  {
    char name[18];
    snprintf(name, sizeof(name), "chameleo-fp-%u", id);
    pthread_setname_np(pthread_self(), name);
  }

  /* Allocate fastpath core context */
  ctx = rte_zmalloc("fast path core context", sizeof(*ctx), 0);
  if (ctx == NULL) 
  {
    LOG_ERROR("allocating fast path core context failed");
    goto error_alloc;
  }
  chameleo_ctx.fp_ctxs[id] = ctx;
  ctx->id = id;

  /* initialize data plane context */
  ret = fast_path_context_init(ctx);
  if (ret != 0) 
  {
    LOG_ERROR("failed to initialize fast path context");
    goto error_dpctx;
  }

  fast_path_loop(ctx);
  fast_path_context_destroy(ctx);

  return 0;

error_dpctx:
  fast_path_context_destroy(ctx);
error_alloc:
  abort();
  return -1;
}
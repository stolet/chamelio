#define _GNU_SOURCE
#include <stdint.h>
#include <pthread.h>

#include <rte_malloc.h>

#include "chameleo.h"
#include "config/config.h"
#include "fast/fast.h"
#include "slow/slow.h"
#include "nic/nic.h"
#include "queue/queue.h"
#include "../utils/log/log.h"

/* TODO: Make this a parameter in the start */
#define QUEUE_SIZE 256

struct chameleo_context cham_ctx;

static int fast_thread(void *arg);
static int fast_start(struct configuration *config);

int main (int argc, char **argv)
{
  int i, j, ret;
  struct configuration *config;
  struct slow_context *s_ctx;
  struct queue *q;
  struct queue **fast_slow_qs, **slow_fast_qs;
  struct fast_context **f_ctxs;
  unsigned threads_launched;

  /* Parse command line options */
  config = &cham_ctx.config;
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
  ret = nic_init(&cham_ctx.nic_ctx, config);
  if (ret != 0)
  {
    LOG_ERROR("failed to initialize nic");
    goto error_nic;
  }

  /* Initialize fast path contexts in hugepages */
  f_ctxs = rte_calloc("fast path context list", 
      cham_ctx.config.fp_cores_max, sizeof(*f_ctxs), 64);
  if (f_ctxs == NULL)
  {
    LOG_ERROR("failed to allocated f_ctxs");
    goto error_nic;
  }
  cham_ctx.f_ctxs = f_ctxs;

  /* Start fast-path threads */
  threads_launched = fast_start(&cham_ctx.config);
  if (threads_launched < cham_ctx.config.fp_cores_max)
  {
    LOG_ERROR("failed to initialize fast path launched=%d target=%d", 
        threads_launched, cham_ctx.config.fp_cores_max);
    goto error_nic;
  }

  s_ctx = malloc(sizeof(struct slow_context));
  if (s_ctx == NULL)
  {
    LOG_ERROR("failed to allocate slow path context");
    goto error_nic;
  }

  fast_slow_qs = malloc(sizeof(struct queue *) * config->fp_cores_max);
  if (fast_slow_qs == NULL)
  {
    LOG_ERROR("failed to allocate list for fast-path to slow-path queues");
    goto error_fast_slow_list;
  }
  s_ctx->fast_slow_qs = fast_slow_qs;
  cham_ctx.fast_slow_qs = fast_slow_qs;
  
  slow_fast_qs = malloc(sizeof(struct queue *) * config->fp_cores_max);
  if (slow_fast_qs == NULL)
  {
    LOG_ERROR("failed to allocate list for slow-path to fast-path queues");
    goto error_slow_fast_list;
  }
  s_ctx->slow_fast_qs = slow_fast_qs;
  cham_ctx.slow_fast_qs = slow_fast_qs;

  /* Create pair of queues for each core */
  for (i = 0; i < config->fp_cores_max; i++)
  {
    q = queue_new(QUEUE_SIZE);
    if (q == NULL)
    {
      LOG_ERROR("failed to allcoate fast to slow queue");
      goto error_queues;
    }
    fast_slow_qs[i] = q;

    q = queue_new(QUEUE_SIZE);
    if (q == NULL)
    {
      LOG_ERROR("failed to allcoate slow to fast queue");
      goto error_queues;
    }
    slow_fast_qs[i] = q;
  }

  /* Initialize slow-path */
  slow_context_init(s_ctx, config->fp_cores_max, fast_slow_qs, slow_fast_qs);

  /* Loop in slow-path */
  slow_loop(s_ctx);

error_queues:
  for (j = 0; j < i; j++)
  {
    free(fast_slow_qs[i]);
    free(slow_fast_qs[i]);
  }
error_slow_fast_list:
  free(fast_slow_qs);
error_fast_slow_list:
  free(s_ctx);
error_nic:
  nic_cleanup(&cham_ctx.nic_ctx);
error_exit:
  return -1;
}

int fast_start(struct configuration *config)
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
    
      if (rte_eal_remote_launch(fast_thread, arg, core) != 0) 
      {
        LOG_ERROR("failed to launch fast path thread");
        return -1;
      }

      threads_launched++;
    }
  }

  return threads_launched;
}

static int fast_thread(void *arg)
{
  int ret;
  uint16_t id = (uintptr_t) arg;
  struct fast_context *f_ctx;

  {
    char name[18];
    snprintf(name, sizeof(name), "chameleo-fp-%u", id);
    pthread_setname_np(pthread_self(), name);
  }

  /* Allocate fastpath core context */
  f_ctx = rte_zmalloc("fast path core context", sizeof(*f_ctx), 0);
  if (f_ctx == NULL) 
  {
    LOG_ERROR("allocating fast path core context failed");
    goto error_alloc;
  }
  cham_ctx.f_ctxs[id] = f_ctx;
  f_ctx->id = id;

  /* initialize data plane context */
  ret = fast_context_init(f_ctx, &cham_ctx.nic_ctx.eth_dev_info,
      cham_ctx.fast_slow_qs[id], cham_ctx.slow_fast_qs[id],
      &cham_ctx.config, id, cham_ctx.nic_ctx.port_id);
  if (ret != 0) 
  {
    LOG_ERROR("failed to initialize fast path context");
    goto error_dpctx;
  }

  fast_loop(f_ctx);
  fast_context_destroy(f_ctx);

  return 0;

error_dpctx:
  fast_context_destroy(f_ctx);
error_alloc:
  abort();
  return -1;
}
#define _GNU_SOURCE
#include <stdint.h>
#include <pthread.h>

#include <rte_malloc.h>

#include "chamelio.h"
#include "config.h"
#include "fast.h"
#include "slow.h"
#include "nic.h"
#include "queue.h"
#include "log.h"
#include "shm.h"
#include "shmalloc.h"


struct chamelio_context cham_ctx;

static int fast_thread(void *arg);
static int fast_start(struct configuration *config);

int main (int argc, char **argv)
{
  int i, j, ret, sfd;
  void *shm_base;
  struct configuration *config;
  struct slow_context *s_ctx;
  struct shm_handle *sh, **fs_handles, **sf_handles;
  struct fast_context **f_ctxs;
  struct shm_allocator *alloc;
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
    goto cleanup_nic;
  }

  /* Initialize fast path contexts in hugepages */
  f_ctxs = rte_calloc("fast path context list", 
      cham_ctx.config.fp_cores_max, sizeof(*f_ctxs), 64);
  if (f_ctxs == NULL)
  {
    LOG_ERROR("failed to allocated f_ctxs");
    goto cleanup_nic;
  }
  cham_ctx.f_ctxs = f_ctxs;

  /* Create internal shared memory region */
  shm_base = shm_create_huge(CHAMELIO_SHM_NAME_INTERNAL, 
      config->shm_internal_len, NULL, &sfd);
  if (shm_base == NULL)
  {
    LOG_ERROR("failed to initialise internal shared memory region");
    goto free_fctxs;
  }

  /* Create allocator for internal shared memory region */
  alloc = shmalloc_init(sfd, shm_base, 1024 * 1024 * 1024);
  if (alloc == NULL)
  {
    LOG_ERROR("failed to initialise shared memory allocator");
    goto destroy_huge;
  }

  /* Create slow-path context */
  s_ctx = malloc(sizeof(struct slow_context));
  if (s_ctx == NULL)
  {
    LOG_ERROR("failed to allocate slow path context");
    goto free_alloc;
  }

  /* Allocate fast->slow queues */
  fs_handles = malloc(sizeof(struct shm_handle *) * config->fp_cores_max);
  if (fs_handles == NULL)
  {
    LOG_ERROR("failed to allocate list for fast to slow queue handles");
    goto free_sctx;
  }
  cham_ctx.fast_slow_handles = fs_handles;
  
  /* Allocate slow->fast queues */
  sf_handles = malloc(sizeof(struct queue *) * config->fp_cores_max);
  if (sf_handles == NULL)
  {
    LOG_ERROR("failed to allocate list for slow-path to fast-path queues");
    goto free_sh_fast_slow_list;
  }
  cham_ctx.slow_fast_handles = sf_handles;

  /* Allocate memory for queues between the slow path and the fast path */
  for (i = 0; i < config->fp_cores_max; i++)
  {
    ret = shmalloc_alloc(alloc, config->cham_queue_len, &sh);
    if (ret != 0)
    {
      LOG_ERROR("failed to allocated memory in shared memory for fast to slow queue");
      goto free_sh_slow_fast_list;
    }
    memset(sh->addr, 0, config->cham_queue_len);
    fs_handles[i] = sh;

    ret = shmalloc_alloc(alloc, config->cham_queue_len, &sh);
    if (ret != 0)
    {
      LOG_ERROR("failed to allocated memory in shared memory for slow to fast queue");
      goto free_sh_slow_fast_list;
    }
    memset(sh->addr, 0, config->cham_queue_len);
    sf_handles[i] = sh;
  }

  /* Initialize slow-path */
  slow_context_init(s_ctx, config, fs_handles, sf_handles);
  
  /* Start fast-path threads */
  threads_launched = fast_start(&cham_ctx.config);
  if (threads_launched < cham_ctx.config.fp_cores_max)
  {
    LOG_ERROR("failed to initialize fast path launched=%d target=%d", 
        threads_launched, cham_ctx.config.fp_cores_max);
    goto free_shs;
  }

  /* Loop in slow-path */
  slow_loop(s_ctx);

free_shs:
  for (j = 0; j < i; j++)
  {
    shmalloc_free(alloc, fs_handles[i]);
    shmalloc_free(alloc, sf_handles[i]);
  }
free_sh_slow_fast_list:
  free(fs_handles);
free_sh_fast_slow_list:
  free(sf_handles);
free_sctx:
  free(s_ctx);
free_alloc:
  free(alloc);
destroy_huge:
  shm_destroy_huge(CHAMELIO_SHM_NAME_INTERNAL, 
      1024 * 1024 * 1024, shm_base, sfd);
free_fctxs:
  rte_free(f_ctxs);
cleanup_nic:
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
    snprintf(name, sizeof(name), "chamelio-fp-%u", id);
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
      cham_ctx.fast_slow_handles[id], cham_ctx.slow_fast_handles[id],
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
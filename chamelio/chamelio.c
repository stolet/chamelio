#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <linux/types.h>
#include <pthread.h>

#include <rte_malloc.h>

#include "chamelio.h"
#include "config.h"
#include "fast.h"
#include "control.h"
#include "nic.h"
#include "queue_types.h"
#include "log.h"
#include "shm.h"
#include "shmalloc.h"
#include "netvirt.h"


struct chamelio_context cham_ctx;

static int fast_thread(void *arg);
static int fast_start(struct configuration *config);

int main (int argc, char **argv)
{
  int i, j, ret, sfd;
  void *shm_base;
  struct configuration *config;
  struct control_context *c_ctx;
  struct shm_handle *sh, **fc_handles, **cf_handles, **txq_handles;
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
      cham_ctx.config.fp_cores_max, sizeof(*f_ctxs), 0);
  if (f_ctxs == NULL)
  {
    LOG_ERROR("failed to allocated f_ctxs");
    goto cleanup_nic;
  }
  cham_ctx.f_ctxs = f_ctxs;

  /* Create internal shared memory region */
  shm_base = shm_create_huge(CHAMELIO_SHM_NAME_INTERNAL, 
      config->shm_internal_len, NULL, &sfd, config->numa_shm_internal);
  if (shm_base == NULL)
  {
    LOG_ERROR("failed to initialise internal shared memory region");
    goto free_fctxs;
  }
  cham_ctx.shm_fd_internal = sfd;
  cham_ctx.shm_base_internal = shm_base;

  /* Create allocator for internal shared memory region */
  alloc = shmalloc_init(shm_base, config->shm_internal_len);
  if (alloc == NULL)
  {
    LOG_ERROR("failed to initialise shared memory allocator");
    goto destroy_huge;
  }

  /* Create control-path context */
  c_ctx = malloc(sizeof(struct control_context));
  if (c_ctx == NULL)
  {
    LOG_ERROR("failed to allocate control path context");
    goto free_alloc;
  }

  /* Parse network virtualization configuration */
  netvirt_table_init(&cham_ctx.inner_table);
  netvirt_table_init(&cham_ctx.gid_table);
  c_ctx->inner_table = &cham_ctx.inner_table;
  c_ctx->gid_table = &cham_ctx.gid_table;
  if (config->virt_gre)
  {
    ret = netvirt_parser(&cham_ctx.inner_table, 
        &cham_ctx.gid_table, config->virt_path);
    if (ret != 0)
    {
      LOG_ERROR("failed to parse network virtualization config");
      goto free_cctx;
    }
  }

  /* Allocate fast->control queues */
  fc_handles = rte_malloc("fast->control handle", 
      sizeof(struct shm_handle *) * config->fp_cores_max, 0);
  if (fc_handles == NULL)
  {
    LOG_ERROR("failed to allocate list for fast to control queue handles");
    goto free_cctx;
  }
  cham_ctx.fast_ctl_handles = fc_handles;
  
  /* Allocate control->fast queues */
  cf_handles = rte_malloc("control->fast handle", 
      sizeof(struct shm_handle *) * config->fp_cores_max, 0);
  if (cf_handles == NULL)
  {
    LOG_ERROR("failed to allocate list for control-path to fast-path handles");
    goto free_sh_fast_control_list;
  }
  cham_ctx.ctl_fast_handles = cf_handles;
  
  /* Allocate control tx queues */
  txq_handles = rte_malloc("tx queue handles", 
      sizeof(struct shm_handle *) * config->fp_cores_max, 0);
  if (txq_handles == NULL)
  {
    LOG_ERROR("failed to allocate list for tx queue handles");
    goto free_sh_control_fast_list;
  }
  cham_ctx.ctxq_handles = txq_handles;

  /* Allocate memory for queues between the control path and the fast path */
  for (i = 0; i < config->fp_cores_max; i++)
  {
    ret = shmalloc_alloc(alloc, config->cham_queue_len * 
        sizeof(struct queue_entry), &sh);
    if (ret != 0)
    {
      LOG_ERROR("failed to allocated memory in"
          "shared memory for fast to control queue");
      goto free_sh_txq_list;
    }
    memset(sh->addr, 0, config->cham_queue_len);
    fc_handles[i] = sh;

    ret = shmalloc_alloc(alloc, config->cham_queue_len * 
        sizeof(struct queue_entry), &sh);
    if (ret != 0)
    {
      LOG_ERROR("failed to allocated memory in" 
          "shared memory for control to fast queue");
      goto free_sh_txq_list;
    }
    memset(sh->addr, 0, config->cham_queue_len);
    cf_handles[i] = sh;
    
    ret = shmalloc_alloc(alloc, 
        config->control_txq_len * config->control_txq_pkt_len, &sh);
    if (ret != 0)
    {
      LOG_ERROR("failed to allocate memory in"
          "shared memory for control txqs");
      goto free_sh_txq_list;
    }
    memset(sh->addr, 0, config->control_txq_len);
    txq_handles[i] = sh;
  }

  /* Initialize control-path */
  control_context_init(c_ctx, &cham_ctx.nic_ctx, 
      config, fc_handles, cf_handles, txq_handles);
  
  /* Start fast-path threads */
  threads_launched = fast_start(&cham_ctx.config);
  if (threads_launched < cham_ctx.config.fp_cores_max)
  {
    LOG_ERROR("failed to initialize fast path launched=%d target=%d", 
        threads_launched, cham_ctx.config.fp_cores_max);
    goto free_shs;
  }

  /* Loop in control-path */
  control_loop(c_ctx);

free_shs:
  for (j = 0; j < i; j++)
  {
    shmalloc_free(alloc, fc_handles[i]);
    shmalloc_free(alloc, cf_handles[i]);
    shmalloc_free(alloc, txq_handles[i]);
  }
free_sh_txq_list:
  rte_free(txq_handles);
free_sh_control_fast_list:
  rte_free(fc_handles);
free_sh_fast_control_list:
  rte_free(cf_handles);
free_cctx:
  free(c_ctx);
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

  __u32 fp_cores_max = config->fp_cores_max;

  /* fast path cores + one control path core */
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
  __u16 id = (uintptr_t) arg;
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

  /* Add GRE and IP configuration tables if using net virtualization */
  if (cham_ctx.config.virt_gre)
  {
    f_ctx->inner_table = &cham_ctx.inner_table;
    f_ctx->gid_table = &cham_ctx.gid_table;
  }
  else
  {
    f_ctx->inner_table = NULL;
    f_ctx->gid_table = NULL;
  }

  /* initialize data plane context */
  ret = fast_context_init(f_ctx, &cham_ctx.nic_ctx, id,
      cham_ctx.fast_ctl_handles[id], cham_ctx.ctl_fast_handles[id],
      cham_ctx.ctxq_handles[id], &cham_ctx.config, 
      cham_ctx.shm_fd_internal, cham_ctx.shm_base_internal);
  if (ret != 0) 
  {
    LOG_ERROR("failed to initialize fast path context");
    goto error_alloc;
  }

  fast_loop(f_ctx);

  return 0;

error_alloc:
  abort();
  return -1;
}

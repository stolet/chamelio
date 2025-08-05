#include <stdlib.h>
#include <stdint.h>
#include <assert.h>

#include "slow.h"
#include "shmalloc.h"
#include "guestif.h"
#include "appif.h"
#include "config.h"
#include "log.h"
#include "queue.h"

static int poll_fast(struct slow_context *ctx);
static int poll_app_contexts(struct slow_context *ctx);
static int handle_new_buf(struct slow_context *ctx, struct queue_entry *qe, 
    struct app_context_slow *actx);

int slow_context_init(struct slow_context *ctx, struct configuration *config,
    struct shm_handle **fs_handles, struct shm_handle **sf_handles)
{
  int i, j;
  struct guest_slow *guests;
  struct app_slow *apps;
  struct app_context_slow *actxs;
  struct equeue *sfq;
  struct dqueue *fsq;
  struct dqueue **fast_slow_qs;
  struct equeue **slow_fast_qs;

  ctx->config = config;
  
  ctx->guest_uxfd = -1;
  ctx->guest_epfd = -1;
  
  ctx->app_uxfd = -1;
  ctx->app_epfd = -1;

  /* Allocate pointer list for queues */
  fast_slow_qs = malloc(sizeof(struct dqueue *) * config->fp_cores_max);
  if (fast_slow_qs == NULL)
  {
    LOG_ERROR("failed to allocate list of fast->slow queues");
    return -1;
  }
  ctx->fast_slow_qs = fast_slow_qs;

  slow_fast_qs = malloc(sizeof(struct equeue *) * config->fp_cores_max);
  if (slow_fast_qs == NULL)
  {
    LOG_ERROR("failed to alloacate list of slow->fast queues");
    goto free_fast_slow_list;
  }
  ctx->slow_fast_qs = slow_fast_qs;

  /* Create a queue with each shared memory handle */
  for (i = 0; i < config->fp_cores_max; i++)
  {
    sfq = equeue_new(config->cham_queue_len, 
        sf_handles[i]->addr, sf_handles[i]->off);
    if (sfq == NULL)
    {
      LOG_ERROR("failed to create fast to slow path queue");
      goto free_slow_fast_list;
    }
    ctx->slow_fast_qs[i] = sfq;

    fsq = dqueue_new(config->cham_queue_len, 
        fs_handles[i]->addr, fs_handles[i]->off);
    if (fsq == NULL)
    {
      LOG_ERROR("failed to create slow to fast path queue");
      goto free_slow_fast_list;
    }
    ctx->fast_slow_qs[i] = fsq;
  }

  /* Allocate guests */
  guests = calloc(config->max_guests, sizeof(struct guest_slow));
  if (guests == NULL)
  {
    LOG_ERROR("failed to allocate guest list");
    goto free_slow_fast_list;
  }
  ctx->guests = guests;
  ctx->n_guests = 0;

  for (i = 0; i < config->max_guests; i++)
  {
    apps = calloc(config->max_apps, sizeof(struct app_slow));
    if (apps == NULL)
    {
      LOG_ERROR("failed to allocate app list for guest=%d", i);
      goto free_guests;
    }
    ctx->guests[i].apps = apps;
    ctx->guests[i].n_apps = 0;

    for (j = 0; j < config->max_app_ctxs; j++)
    {
      actxs = calloc(config->max_app_ctxs, sizeof(struct app_context_slow));
      if (actxs == NULL)
      {
        LOG_ERROR("failed to allocated app context list"
            "for guest=%d app=%d", i, j);
        goto free_guests;
      }
      ctx->guests[i].apps[j].ctxs = actxs;
      ctx->guests[i].apps[j].n_ctxs = 0;
    }
  }

  return 0;

free_guests:
  free(guests);
free_slow_fast_list:
  free(slow_fast_qs);
free_fast_slow_list:
  free(fast_slow_qs);
  return -1;
}

int slow_loop(struct slow_context *ctx)
{
  int ret;

  ret = guestif_init(ctx);
  if (ret != 0)
  {
    LOG_ERROR("failed to initialise guestif");
    return -1;
  }

  ret = appif_init(ctx);
  if (ret != 0)
  {
    LOG_ERROR("failed to initialise appif");
    return -1;
  }

  while (1) 
  {
    guestif_poll(ctx);
    appif_poll(ctx);
    poll_fast(ctx);
    poll_app_contexts(ctx);
  }
}

static int poll_fast(struct slow_context *ctx)
{
  struct dqueue *q;
  struct queue_entry *qe;
  int i;

  for (i = 0; i < ctx->config->fp_cores_max; i++)
  {
    /* TODO: Dequeue in batches to improve performance and prevent us
       from spending too much time in one core */
    q = ctx->fast_slow_qs[i];
    qe = queue_head(q);

    if (qe != NULL)
    {
      switch (qe->type)
      {
        case QUEUE_EMPTY:
          break;
        case QUEUE_ARP_TX:
          break;
        case QUEUE_ARP_RX:
          break;
        default:
          LOG_ERROR("unknown queue entry type from "
              "fast path to slow path type=%d", qe->type);
          abort();
      }
    }
  }

  return 0;
}

static int poll_app_contexts(struct slow_context *ctx)
{
  int i, j, z;
  struct dqueue *q;
  struct queue_entry *qe;
  struct guest_slow *g;
  struct app_slow *a;
  struct app_context_slow *actx;

  for (i = 0; i < ctx->n_guests; i++)
  {
    g = &ctx->guests[i];
    for (j = 0; j < g->n_apps; j++)
    {
      a = &g->apps[j];
      for (z = 0; z < a->n_ctxs; z++)
      {
        actx = &a->ctxs[z];
        q = actx->app_cham_q;
        qe = queue_head(q);

        switch (qe->type)
        {
          case QUEUE_NEW_BUF:
            handle_new_buf(ctx, qe, actx);
            queue_dequeue(q);
            break;
          default:
            LOG_ERROR("unknown queue entry type from "
              "app ctx to slow path type=%d", qe->type);
            return -1;
        }
      }
    }
  }

  return 0;
}

static int handle_new_buf(struct slow_context *ctx, struct queue_entry *qe, 
    struct app_context_slow *actx)
{
  int ret;
  struct equeue *q;
  struct shm_handle *sh;
  struct shm_allocator *alloc;
  struct queue_new_buf_res *res;
  struct queue_new_buf_req *req_app;

  req_app = (struct queue_new_buf_req *) &qe->data;

  /* Allocate space for buffer from shared memory region */
  alloc = actx->app->guest->alloc;
  /* TODO: Don't always pass the rxbuf len. Differentiate TX and RX bufs */
  ret = shmalloc_alloc(alloc, ctx->config->rxbuf_len, &sh);
  if (ret != 0)
  {
    LOG_ERROR("failed to allocate buffer in shm");
    return -1;
  }

  /* Send response to application */
  q = actx->cham_app_q;
  qe = queue_tail(q);
  res = (struct queue_new_buf_res *) &qe->data;
  res->opaque = req_app->opaque;
  res->base = sh->off;
  res->len = ctx->config->rxbuf_len;
  queue_enqueue(q, QUEUE_NEW_BUF);

  return 0;
}
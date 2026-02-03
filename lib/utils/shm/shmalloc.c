#include <stdio.h>
#include <stdlib.h>
#include <linux/types.h>

#include "shmalloc.h"
#include "log.h"

#define SHMALLOC_ALIGN 64
#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

static inline struct shm_handle *sh_alloc(void);
static inline void sh_free(struct shm_handle *sh);
static inline void merge_items(struct shm_allocator *alloc, 
    struct shm_handle *sh_prev);

struct shm_allocator * shmalloc_init(void *shm_base, __u64 len)
{
  struct shm_allocator *alloc;
  struct shm_handle *sh;

  if ((sh = sh_alloc()) == NULL) 
  {
    LOG_ERROR("sh_alloc failed");
    return NULL;
  }

  sh->addr = shm_base;
  sh->off = 0;
  sh->len = len;
  sh->next = NULL;

  alloc = malloc(sizeof(struct shm_allocator));
  if (alloc == NULL)
  {
    LOG_ERROR("failed to allocate shared memory allocator");
    return NULL;
  }
  alloc->freelist = sh;
  alloc->shm_base = shm_base;

  return alloc;
}

int shmalloc_alloc(struct shm_allocator *alloc, size_t length, 
    struct shm_handle **handle)
{
  struct shm_handle *sh, *sh_prev, *ph_new, *sh_suffix;
  __u64 aligned_off;
  size_t prefix_len, suffix_len;

  length = ALIGN_UP(length, SHMALLOC_ALIGN);

  /* look for first fit with aligned start */
  sh_prev = NULL;
  sh = alloc->freelist;
  while (sh != NULL)
  {
    aligned_off = ALIGN_UP(sh->off, SHMALLOC_ALIGN);
    if (aligned_off < sh->off)
    {
      sh_prev = sh;
      sh = sh->next;
      continue;
    }

    prefix_len = aligned_off - sh->off;
    if (prefix_len > sh->len || prefix_len + length > sh->len)
    {
      sh_prev = sh;
      sh = sh->next;
      continue;
    }

    suffix_len = sh->len - prefix_len - length;

    if (prefix_len == 0 && suffix_len == 0)
    {
      /* exact fit, remove from freelist */
      if (sh_prev == NULL)
        alloc->freelist = sh->next;
      else
        sh_prev->next = sh->next;
      ph_new = sh;
    }
    else
    {
      /* need to split */
      ph_new = sh_alloc();
      if (ph_new == NULL)
      {
        LOG_ERROR("sh_alloc failed");
        return -1;
      }

      ph_new->addr = alloc->shm_base + aligned_off;
      ph_new->off = aligned_off;
      ph_new->len = length;
      ph_new->next = NULL;

      if (prefix_len == 0)
      {
        /* reuse current free block as suffix */
        sh->off = aligned_off + length;
        sh->addr = alloc->shm_base + sh->off;
        sh->len = suffix_len;
      }
      else
      {
        /* keep current block as prefix */
        if (suffix_len > 0)
        {
          sh_suffix = sh_alloc();
          if (sh_suffix == NULL)
          {
            sh_free(ph_new);
            LOG_ERROR("sh_alloc failed");
            return -1;
          }

          sh_suffix->off = aligned_off + length;
          sh_suffix->addr = alloc->shm_base + sh_suffix->off;
          sh_suffix->len = suffix_len;
          sh_suffix->next = sh->next;
          sh->len = prefix_len;
          sh->next = sh_suffix;
        }
        else
        {
          sh->len = prefix_len;
        }
      }
    }

    *handle = ph_new;
    return 0;
  }

  /* didn't find a fit */
  return -1;
}

void shmalloc_free(struct shm_allocator *alloc, struct shm_handle *handle)
{
  struct shm_handle *sh, *sh_prev;

  /* look for first successor */
  sh_prev = NULL;
  sh = alloc->freelist;
  while (sh != NULL && sh->next != NULL && sh->next->off < handle->off) 
  {
    sh_prev = sh;
    sh = sh->next;
  }

  /* add to list */
  if (sh_prev == NULL) 
  {
    handle->next = alloc->freelist;
    alloc->freelist = handle;
  } 
  else 
  {
    handle->next = sh_prev->next;
    sh_prev->next = handle;
  }

  /* merge items if necessary */
  merge_items(alloc, sh_prev);
}

/** Merge handles around newly inserted item (pointer to predecessor or NULL
 * passed).
 */
static inline void merge_items(struct shm_allocator *alloc, 
    struct shm_handle *sh_prev)
{
  struct shm_handle *sh, *ph_next;

  /* try to merge with predecessor if there is one */
  if (sh_prev != NULL) 
  {
    sh = sh_prev->next;
    if (sh_prev->off + sh_prev->len == sh->off) 
    {
      /* merge with predecessor */
      sh_prev->next = sh->next;
      sh_prev->len += sh->len;
      sh_free(sh);
      sh = sh_prev;
    }
  } 
  else 
  {
    sh = alloc->freelist;
  }

  /* try to merge with successor if there is one */
  ph_next = sh->next;
  if (ph_next != NULL && sh->off + sh->len == ph_next->off) 
  {
    sh->len += ph_next->len;
    sh->next = ph_next->next;
    sh_free(ph_next);
  }
}

static inline struct shm_handle *sh_alloc(void)
{
  return malloc(sizeof(struct shm_handle));
}

static inline void sh_free(struct shm_handle *sh)
{
  free(sh);
}

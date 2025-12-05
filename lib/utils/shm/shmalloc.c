#include <stdio.h>
#include <stdlib.h>
#include <linux/types.h>

#include "shmalloc.h"
#include "log.h"

static inline struct shm_handle *sh_alloc(void);
static inline void sh_free(struct shm_handle *sh);
static inline void merge_items(struct shm_allocator *alloc, 
    struct shm_handle *sh_prev);

struct shm_allocator * shmalloc_init(int shm_fd, void *shm_base, __u64 len)
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
  alloc->shm_fd = shm_fd;
  alloc->shm_base = shm_base;

  return alloc;
}

int shmalloc_alloc(struct shm_allocator *alloc, size_t length, 
    struct shm_handle **handle)
{
  struct shm_handle *sh, *sh_prev, *ph_new;

  /* look for first fit */
  sh_prev = NULL;
  sh = alloc->freelist;
  while (sh != NULL && sh->len < length) 
  {
    sh_prev = sh;
    sh = sh->next;
  }

  /* didn't find a fit */
  if (sh == NULL) 
    return -1;

  if (sh->len == length) 
  {
    /* simple case, don't need to split this handle */

    /* pointer to previous next pointer for removal */
    if (sh_prev == NULL) 
      alloc->freelist = sh->next;
    else 
      sh_prev->next = sh->next;

    ph_new = sh;
  } 
  else 
  {
    /* need to split */

    /* new packetmem handle for splitting */
    if ((ph_new = sh_alloc()) == NULL) 
    {
      LOG_ERROR("sh_alloc failed");
      return -1;
    }

    ph_new->addr = sh->addr;
    ph_new->off = sh->off;
    ph_new->len = length;
    ph_new->next = NULL;

    sh->off += length;
    sh->addr = alloc->shm_base + sh->off;
    sh->len -= length;
  }

  *handle = ph_new;

  return 0;
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

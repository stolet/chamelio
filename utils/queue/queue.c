#include <stdlib.h>

#include "queue.h"
#include "queue_fns.h"
#include "log.h"
#include "utils.h"
#include "shmalloc.h"

/*  Maybe we want to use the DPDK rte_malloc function here
    so we allocate from hugepages. But this is a bit annoying since
    the library also uses the queue interface but it can't allocate
    from hugepages. */
struct equeue * equeue_new(__u32 nelems, size_t elsize,
    void *addr, __u64 off)
{
  struct equeue *q;

  q = malloc(sizeof(struct equeue));
  if (q == NULL)
  {
    LOG_ERROR("Failed to allocate queue from hugepages");
    return NULL;
  }
  
  q->off = off;
  q->entries = addr;
  q->tail = 0;
  q->nelems = nelems;
  q->elsize = elsize;

  return q;
}

struct dqueue * dqueue_new(__u32 nelems, size_t elsize, 
    void *addr, __u64 off)
{
  struct dqueue *q;

  q = malloc(sizeof(struct dqueue));
  if (q == NULL)
  {
    LOG_ERROR("Failed to allocate queue from hugepages");
    return NULL;
  }
  
  q->off = off;
  q->entries = addr;
  q->head = 0;
  q->nelems = nelems;
  q->elsize = elsize;

  return q;
}

int equeue_init(struct equeue *q, __u32 nelems, size_t elsize,
    void *addr, __u64 off)
{
  if (q == NULL)
    return -1;

  q->off = off;
  q->entries = addr;
  q->tail = 0;
  q->nelems = nelems;
  q->elsize = elsize;

  return 0;
}

int dqueue_init(struct dqueue *q, __u32 nelems, size_t elsize,
    void *addr, __u64 off)
{
  if (q == NULL)
    return -1;

  q->off = off;
  q->entries = addr;
  q->head = 0;
  q->nelems = nelems;
  q->elsize = elsize;

  return 0;
}
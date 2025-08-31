#include "queue.h"
#include "log.h"
#include "utils.h"
#include "shmalloc.h"

/*  Maybe we want to use the DPDK rte_malloc function here
    so we allocate from hugepages. But this is a bit annoying since
    the library also uses the queue interface but it can't allocate
    from hugepages. */
struct equeue * equeue_new(uint32_t nelems, size_t elsize,
    void *addr, uint64_t off)
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

struct dqueue * dqueue_new(uint32_t nelems, size_t elsize, 
    void *addr, uint64_t off)
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

int equeue_init(struct equeue *q, uint32_t nelems, size_t elsize,
    void *addr, uint64_t off)
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

int dqueue_init(struct dqueue *q, uint32_t nelems, size_t elsize,
    void *addr, uint64_t off)
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

/* Only one thread can enqueue */
int queue_enqueue(struct equeue *q, uint8_t type)
{
  uint32_t tail;
  uint8_t *tail_type;

  tail_type = (uint8_t *) q->entries + q->tail;

  /* Queue is full */
  if (*tail_type != QUEUE_EMPTY)
    return -1;

  tail = q->tail + q->elsize;
  if (tail >= (q->elsize * q->nelems))
    tail = 0;
  q->tail = tail;
    
  MEM_BARRIER();
  *tail_type = type;

  return 0;
}

/* Only one thread can dequeue */
int queue_dequeue(struct dqueue *q)
{
  uint32_t head;
  uint8_t *type;
  
  type = (uint8_t *) q->entries + q->head;

  /* Queue is empty */
  if (*type == QUEUE_EMPTY)
    return -1;

  head = q->head + q->elsize;
  if (head >= (q->elsize * q->nelems))
    head = 0;
  q->head = head;
  
  MEM_BARRIER();
  *type = QUEUE_EMPTY;

  return 0;
}

void * queue_head(struct dqueue *q)
{
  uint8_t *type;
  
  type = (uint8_t *) q->entries + q->head;
  
  /* Queue is empty */
  if (*type == QUEUE_EMPTY)
    return NULL;

  return type;
}

void * queue_tail(struct equeue *q)
{
  uint8_t *type;
  
  type = (uint8_t *) q->entries + q->tail;
  
  /* Queue is full */
  if (*type != QUEUE_EMPTY)
    return NULL;

  return type;
}
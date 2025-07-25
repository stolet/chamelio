#include <rte_malloc.h>

#include "queue.h"

#include "log.h"
#include "utils.h"
#include "shmalloc.h"

struct queue * queue_new(uint32_t size, struct shm_allocator *alloc)
{
  int ret;
  struct queue *q;
  struct shm_handle *sh;

  q = rte_malloc("queue", sizeof(struct queue), 0);
  if (q == NULL)
  {
    LOG_ERROR("Failed to allocate queue from hugepages");
    return NULL;
  }
  
  ret = shmalloc_alloc(alloc, size, &sh);
  if (ret != 0)
  {
    LOG_ERROR("failed to allocated memory in shared memory");
    goto error_entries;
  }
  q->sh = sh;
  q->entries = alloc->shm_base + sh->base;
  q->head = 0;
  q->tail = 0;
  q->size = size;

  return q;

error_entries:
  free(q);
  return NULL;
}

/* Only one thread can enqueue */
int queue_enqueue(struct queue *q, uint8_t type)
{
  uint32_t tail;
  struct queue_entry *qe = q->entries + q->tail;
  
  /* Queue is full */
  if (qe->type != QUEUE_EMPTY)
    return -1;

  tail = q->tail + sizeof(struct queue_entry);
  if (tail > q->size)
    tail = 0;
  q->tail = tail;
    
  MEM_BARRIER();
  qe->type = type;

  return 0;
}

/* Only one thread can dequeue */
int queue_dequeue(struct queue *q)
{
  uint32_t head;
  struct queue_entry *qe = q->entries + q->head;

  /* Queue is empty */
  if (qe->type == QUEUE_EMPTY)
    return -1;

  head = q->head + sizeof(struct queue_entry);
  if (head > q->size)
    head = 0;
  q->head = head;

  MEM_BARRIER();
  qe->type = 0;

  return 0;
}

struct queue_entry * queue_head(struct queue *q)
{
  struct queue_entry *qe;
  qe = (void *) q->entries + q->head;
  
  /* Queue is empty */
  if (qe->type == QUEUE_EMPTY)
    return NULL;

  return qe;
}
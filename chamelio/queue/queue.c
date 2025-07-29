#include <rte_malloc.h>

#include "queue.h"

#include "log.h"
#include "utils.h"
#include "shmalloc.h"

struct equeue * equeue_new(uint32_t size, struct shm_handle *sh)
{
  struct equeue *q;

  q = rte_malloc("queue", sizeof(struct equeue), 0);
  if (q == NULL)
  {
    LOG_ERROR("Failed to allocate queue from hugepages");
    return NULL;
  }
  
  q->sh = sh;
  q->entries = sh->addr;
  q->tail = 0;
  q->size = size;

  return q;
}

struct dqueue * dqueue_new(uint32_t size, struct shm_handle *sh)
{
  struct dqueue *q;

  q = rte_malloc("queue", sizeof(struct dqueue), 0);
  if (q == NULL)
  {
    LOG_ERROR("Failed to allocate queue from hugepages");
    return NULL;
  }
  
  q->sh = sh;
  q->entries = sh->addr;
  q->head = 0;
  q->size = size;

  return q;
}

/* Only one thread can enqueue */
int queue_enqueue(struct equeue *q, struct queue_entry *qe)
{
  uint32_t tail;
  struct queue_entry *tail_entry = q->entries + q->tail;
  
  /* Queue is full */
  if (tail_entry->type != QUEUE_EMPTY)
    return -1;

  /* TODO: This copy could be a performance bottleneck. Leave it here
     for now unless we notice it is a problem. An alternative is to
     expose a queue_head and queue_tail function so that the caller
     can directly modify the returned queue_entry. The enqueue
     and dequeue function would then just increment the tail and
     head. Rename it to increment_head and increment_tail. */
  memcpy(&tail_entry->data, &qe->data, sizeof(qe->data));

  tail = q->tail + sizeof(struct queue_entry);
  if (tail > q->size)
    tail = 0;
  q->tail = tail;
    
  MEM_BARRIER();
  tail_entry->type = qe->type;

  return 0;
}

/* Only one thread can dequeue */
int queue_dequeue(struct dqueue *q)
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

struct queue_entry * queue_head(struct dqueue *q)
{
  struct queue_entry *qe;
  qe = (void *) q->entries + q->head;
  
  /* Queue is empty */
  if (qe->type == QUEUE_EMPTY)
    return NULL;

  return qe;
}
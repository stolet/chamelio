#include "queue.h"
#include "log.h"
#include "utils.h"
#include "shmalloc.h"

/*  Maybe we want to use the DPDK rte_malloc function here
    so we allocate from hugepages. But this is a bit annoying since
    the library also uses the queue interface but it can't allocate
    from hugepages. */

struct equeue * equeue_new(uint32_t size, void *addr, uint64_t off)
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
  q->size = size;

  return q;
}

struct dqueue * dqueue_new(uint32_t size, void *addr, uint64_t off)
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
  q->size = size;

  return q;
}

/* Only one thread can enqueue */
int queue_enqueue(struct equeue *q, uint8_t type)
{
  uint32_t tail;
  struct queue_entry *tail_entry = q->entries + q->tail;
  
  /* Queue is full */
  if (tail_entry->type != QUEUE_EMPTY)
    return -1;

  tail = q->tail + sizeof(struct queue_entry);
  if (tail > q->size)
    tail = 0;
  q->tail = tail;
    
  MEM_BARRIER();
  tail_entry->type = type;

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

struct queue_entry * queue_tail(struct equeue *q)
{
  struct queue_entry *qe;
  qe = (void *) q->entries + q->tail;
  
  /* Queue is empty */
  if (qe->type != QUEUE_EMPTY)
    return NULL;

  return qe;
}
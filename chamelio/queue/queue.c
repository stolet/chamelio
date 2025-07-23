#include <rte_malloc.h>

#include "queue.h"

#include "log.h"
#include "utils.h"

struct queue * queue_new(uint32_t size)
{
  struct queue *q;
  struct queue_entry *q_entries;

  q = rte_malloc("queue", sizeof(struct queue), 0);
  if (q == NULL)
  {
    LOG_ERROR("Failed to allocate queue from hugepages");
    return NULL;
  }

  q_entries = rte_malloc("queue entries", sizeof(struct queue_entry) * size, 0);
  if (q_entries == NULL)
  {
    LOG_ERROR("Failed to allcoate queue entries from hugepages");
    goto error_entries;
  }

  q->head = 0;
  q->tail = 0;
  q->size = size;
  q->entries = q_entries;

  return q;

error_entries:
  free(q);
  return NULL;
}

/* Only one sides enqueue and one side dequeues */
int queue_enqueue(struct queue *q, uint8_t type)
{
  struct queue_entry *qe = &q->entries[q->tail];
  
  /* Queue is full */
  if (qe->type != QUEUE_TYPE_EMPTY)
    return -1;

  qe->type = type;
  
  MEM_BARRIER();
  q->tail = (q->tail + 1) % q->size;

  return 0;
}

int queue_dequeue(struct queue *q)
{
  uint32_t old_head;
  struct queue_entry *qe = &q->entries[q->head];

  /* Queue is empty */
  if (qe->type == QUEUE_TYPE_EMPTY)
    return -1;

  old_head = q->head;
  q->head = (q->head + 1) % q->size;

  MEM_BARRIER();
  q->entries[old_head].type = 0;

  return 0;
}

struct queue_entry * queue_head(struct queue *q)
{
  /* Queue is empty */
  if (q->head == q->tail)
    return NULL;

  return &q->entries[q->head];
}
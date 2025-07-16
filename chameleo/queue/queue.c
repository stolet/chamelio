#include <rte_malloc.h>

#include "queue.h"
#include "../chameleo.h"
#include "../../utils/log/log.h"

struct queue * queue_new(uint32_t n)
{
  struct queue *q;
  struct queue_entry *q_entries;

  q = rte_malloc("queue", sizeof(struct queue), 0);
  if (q == NULL)
  {
    LOG_ERROR("Failed to allocate queue from hugepages");
    return NULL;
  }

  q_entries = rte_malloc("queue entries", sizeof(struct queue_entry) * n, 0);
  if (q_entries == NULL)
  {
    LOG_ERROR("Failed to allcoate queue entries from hugepages");
    goto error_entries;
  }

  q->n = 0;
  q->max = 0;
  q->head = 0;
  q->tail = 0;
  q->entries = q_entries;

  return q;

error_entries:
  free(q);
  return NULL;
}

/* Only one sides enqueue and one side dequeues */
int queue_enqueue(struct queue *q, uint8_t type)
{
  int ret;
  uint32_t new_tail, old_tail;

  if (q->n == q->max)
    return -1;

  old_tail = q->tail;
  new_tail = (q->tail + 1) % q->max;
  ret = __sync_val_compare_and_swap(&q->tail, old_tail, new_tail);
  __sync_fetch_and_add(&q->n, 1);

  if (ret != old_tail)
  {
    LOG_ERROR("Tail didn't get update properly." 
        "This might indicate a synchronization bug");
    abort();
  }

  q->entries[q->tail].type = type;

  return 0;
}

struct queue_entry * queue_dequeue(struct queue *q)
{
  int ret;
  uint32_t new_head, old_head;
  struct queue_entry * qe;

  if (q->n == 0)
    return NULL;

  qe = &q->entries[q->head];
  __sync_fetch_and_sub(&q->n, 1);

  old_head = q->head;
  new_head = (q->head + 1) % q->max;
  ret = __sync_val_compare_and_swap(&q->head, old_head, new_head);

  if (ret != old_head)
  {
    LOG_ERROR("Tail didn't get update properly." 
        "This might indicate a synchronization bug");
    abort();
  }

  return qe;
}
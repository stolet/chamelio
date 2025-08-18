#include "udp_queue.h"
#include "queue.h"
#include "log.h"
#include "shmalloc.h"

/* Only one thread can enqueue */
int udp_queue_enqueue(struct equeue *q, uint8_t type)
{
  uint32_t tail;
  struct udp_queue_entry *tail_entry = q->entries + q->tail;
  
  /* Queue is full */
  if (tail_entry->type != QUEUE_EMPTY)
    return -1;

  tail = q->tail + sizeof(struct udp_queue_entry);
  if (tail > q->size)
    tail = 0;
  q->tail = tail;
    
  MEM_BARRIER();
  tail_entry->type = type;

  return 0;
}

/* Only one thread can dequeue */
int udp_queue_dequeue(struct dqueue *q)
{
  uint32_t head;
  struct udp_queue_entry *qe = q->entries + q->head;

  /* Queue is empty */
  if (qe->type == QUEUE_EMPTY)
    return -1;

  head = q->head + sizeof(struct udp_queue_entry);
  if (head > q->size)
    head = 0;
  q->head = head;

  MEM_BARRIER();
  qe->type = 0;

  return 0;
}

struct udp_queue_entry * udp_queue_head(struct dqueue *q)
{
  struct udp_queue_entry *qe;
  qe = (void *) q->entries + q->head;
  
  /* Queue is empty */
  if (qe->type == QUEUE_EMPTY)
    return NULL;

  return qe;
}

struct udp_queue_entry * udp_queue_tail(struct equeue *q)
{
  struct udp_queue_entry *qe;
  qe = (void *) q->entries + q->tail;
  
  /* Queue is empty */
  if (qe->type != QUEUE_EMPTY)
    return NULL;

  return qe;
}
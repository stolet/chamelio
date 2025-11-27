#ifndef QUEUE_FNS_H_
#define QUEUE_FNS_H_

#include <stddef.h>
#include <linux/types.h>

#include "queue.h"
#include "queue_types.h"
#include "utils.h"

/* Allocates a new queue that can only enqueue entries. 
   Prevents race conditions */
struct equeue * equeue_new(__u32 nelems, size_t elsize, 
    void *addr, __u64 off);
/* Allocates a new queue that can only dequeue entries. 
   Prevents race conditions */
struct dqueue * dqueue_new(__u32 nelems, size_t elsize, 
    void *addr, __u64 off);
/* Initialises the struct for a new equeue */
int equeue_init(struct equeue *q, __u32 nelems, size_t elsize,
    void *addr, __u64 off);
/* Initialises the struct for a new dqueue */
int dqueue_init(struct dqueue *q, __u32 nelems, size_t elsize,
    void *addr, __u64 off);

/****************************************************/
/* Have the functions below as inline because they
   often get called in hot loops */
/****************************************************/

/* Advances the tail pointer for the queue.
   Only one thread can enqueue */
static inline int queue_enqueue(struct equeue *q, __u8 type)
{
  __u32 tail;
  __u8 *tail_type;

  tail_type = (__u8 *) q->entries + q->tail;

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

/* Advances the head pointer for the queue. 
   Only one thread can dequeue */
static inline int queue_dequeue(struct dqueue *q)
{
  __u32 head;
  __u8 *type;
  
  type = (__u8 *) q->entries + q->head;

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

/* Returns a pointer to the queue entry at the head of the queue */
static inline void * queue_head(struct dqueue *q)
{
  __u8 *type;
  
  type = (__u8 *) q->entries + q->head;
  
  /* Queue is empty */
  if (*type == QUEUE_EMPTY)
    return NULL;

  return type;
}

/* Returns a pointer to the next empty queue entry at the tail of the queue */
static inline void * queue_tail(struct equeue *q)
{
  __u8 *type;
  
  type = (__u8 *) q->entries + q->tail;
  
  /* Queue is full */
  if (*type != QUEUE_EMPTY)
    return NULL;

  return type;
}

#endif

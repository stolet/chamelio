#ifndef QUEUE_FNS_H_
#define QUEUE_FNS_H_

#include <stddef.h>
#include <linux/types.h>

#include "queue.h"

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
/* Advances the tail pointer for the queue */
int queue_enqueue(struct equeue *q, __u8 type);
/* Advances the head pointer for the queue */
int queue_dequeue(struct dqueue *q);
/* Returns a pointer to the queue entry at the head of the queue */
void * queue_head(struct dqueue *q);
/* Returns a pointer to the next empty queue entry at the tail of the queue */
void * queue_tail(struct equeue *q);

#endif

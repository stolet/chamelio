#ifndef QUEUE_H_
#define QUEUE_H_

#include <stddef.h>
#include <linux/types.h>

/* Max number of queues a protocol can open */
#define MAX_PROTO_QUEUES 16
#define MAX_PROTO_MAPS 8

/* This queue is only used for enqueuing */
struct equeue {
  /* Only the side that enqueues updates the tail */
  __u32 tail;
  /* Number of elements in the queue */
  __u32 nelems;
  /* Size of each element in the queue */
  size_t elsize;
  /* Offset from the shared memory region to start of the queue */
  __u64 off;
  /* List of entries that points to the start of the queue in the shm region */
  void *entries;
};

/* This queue is only used for dequeueing */
struct dqueue {
  /* Only the side that dequeues updates the head */
  __u32 head;
  /* Number of elements in the queue */
  __u32 nelems;
  /* Size of each element in the queue */
  size_t elsize;
  /* Offset from the shared memory region to start of the queue */
  __u64 off;
  /* List of entries that points to the start of the queue in the shm region */
  void *entries;
};

#endif

#ifndef CHAM_FAST_H
#define CHAM_FAST_H

#include <linux/types.h>

#include "queue.h"
#include "scheduler.h"

/* Max number of fast-path cores */
#define MAX_FP_CORES 16

/* Invalid ID used to represent end of queue list */
#define PROTOQ_ID_INVALID UINT16_MAX

/* Queue structure used by fast-path for enqueue */
struct cham_equeue {
  /* ID of this protocol queue */
  __u16 id;
  /* Queue structure */
  struct equeue eq;
};

/* Queue structure used by fast-path to dequeue */
struct cham_dqueue {
  /* ID of this protocol queue */
  __u16 id;
  /* Queue structure */
  struct dqueue dq;
  /* Next entry in the queue */
  __u16 next;
  /* Previous entry in the queue */
  __u16 prev;
};

/* Map structure used by the fast-path */
struct cham_map {
  /* ID of this map in protocol */
  __u16 id;
  /* Number of elements in the map */
  __u32 nelems;
  /* Size of each element in the map */
  __u32 elsize;
  /* Offset in shared memory where this map starts */
  __u64 off;
  /* Address to start of map entries */
  void *addr;
};

struct cham_proto_handle {
  /* List of protocol queues use by fast-path to enqueue */
  struct cham_equeue equeues[MAX_PROTO_QUEUES]; 
  /* List of created protocol maps */
  struct cham_map maps[MAX_PROTO_MAPS];
  /* TX scheduler for this protocol */
  struct cham_scheduler sched;
  /* Shared memory base */
  void *shm_base;
};

/* Context passed as a parameter to ebpf functions */
struct cham_ebpf_ctx {
  /* Pointer to packet buffer */
  void *pkt;
  /* Queue ID for used for event_deq */
  int qid;
  /* Queue entry dequeued by Chamelio */
  struct queue_entry *qe;
  /* List of protocol queues use by fast-path to enqueue */
  struct cham_equeue equeues[MAX_PROTO_QUEUES];
  /* Number of registered maps */
  __u16 nmaps;
  /* List of created protocol maps */
  struct cham_map maps[MAX_PROTO_MAPS];
  /* TX scheduler for this protocol */
  struct cham_scheduler sched;
  /* Shared memory base */
  void *shm_base;
};

#endif 
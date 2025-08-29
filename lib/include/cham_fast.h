#ifndef CHAM_FAST_H
#define CHAM_FAST_H

#include <stdint.h>

#include "queue.h"

/* Max number of fast-path cores */
#define MAX_FP_CORES 16
/* Max number of entries in the scheduler */
#define MAX_SCHED_ENTRIES (128 * 1024)

/* Invalid ID used to represent end of scheduler list */
#define SCHED_ID_INVALID (-1U)
/* Invalid ID used to represent end of queue list */
#define PROTOQ_ID_INVALID UINT16_MAX

/* Ready entry for data staged before transmission */
struct cham_ready_entry {
  /* ID used by protocol to identify what should send */
  uint64_t id;
  /* How much data should be sent */
  uint32_t ready;
};

/* Queue structure used by fast-path for enqueue */
struct cham_equeue {
  /* ID of this protocol queue */
  uint16_t id;
  /* Queue structure */
  struct equeue eq;
};

/* Queue structure used by fast-path to dequeue */
struct cham_dqueue {
  /* ID of this protocol queue */
  uint16_t id;
  /* Queue structure */
  struct dqueue dq;
  /* Next entry in the queue */
  uint16_t next;
  /* Previous entry in the queue */
  uint16_t prev;
};

/* Entry in the scheduler priority list */
struct cham_sched_entry {
  /* ID for the entry (e.g. flow_id or socket_id) */
  uint32_t id;
  /* Next entry in the priority list */
  uint32_t next_entry;
  /* Priority for the entry */
  uint32_t priority;
  /* Units available for transmission */
  uint32_t avail;
  /* Opaque pointer to a struct that wants to transmit (e.g. socket, flow) */
  uint64_t opaque;
};

/* Map structure used by the fast-path */
struct cham_map {
  /* ID of this map in protocol */
  uint16_t id;
  /* Number of elements in the map */
  uint32_t nelems;
  /* Size of each element in the map */
  uint32_t elsize;
  /* Offset in shared memory where this map starts */
  uint64_t off;
  /* Address to start of map entries */
  void *addr;
};

/* Transmit scheduler that decides what should send next */
struct cham_scheduler {
  /* Pre-allocated array of all entries in the priority list */
  struct cham_sched_entry entries[MAX_SCHED_ENTRIES];
  /* First element of the priority list */
  uint32_t head;
  /* Last element of the priority list */
  uint32_t tail;
};

struct cham_proto_handle {
  /* List of protocol queues use by fast-path to enqueue */
  struct cham_equeue equeues[MAX_PROTO_QUEUES]; 
  /* List of created protocol maps */
  struct cham_map maps[MAX_PROTO_MAPS];
  /* TX scheduler for this protocol */
  struct cham_scheduler sched;
};

#endif 
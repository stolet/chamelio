#ifndef SCHED_H_
#define SCHED_H_

#include <stdint.h>

#include "queue.h"

/* Invalid ID used to represent end of list */
#define SCHED_ID_INVALID (-1U)
/* Max number of entries in the scheduler */
#define MAX_SCHED_ENTRIES (128 * 1024)

/* Entry in the scheduler priority list */
struct sched_entry {
  /* Identifier for whatever is being scheduled (e.g. socket, flow) */
  uint64_t id;
  /* Next entry in the priority list */
  uint32_t next_entry;
  /* Priority for the entry */
  uint32_t priority;
  /* Units available for transmission */
  uint32_t avail;
};

/* Transmit scheduler that decides what should send next */
struct scheduler {
  /* Pre-allocated array of all entries in the priority list */
  struct sched_entry entries[MAX_SCHED_ENTRIES];
  /* First element of the priority list */
  uint32_t head;
  /* Last element of the priority list */
  uint32_t tail;
  /* Head of free list entries */
  uint32_t free_head;
};

/* Initialises the scheduler for a protocol */
void sched_init(struct scheduler *sched);
/* Adds an entry to the queue manager priority list */
int sched_add(struct scheduler *sched, struct sched_entry *entry);
/* Removes an entry from the queue manager priority list */
int sched_remove(struct scheduler *sched, uint32_t id);
/* Peeks at the first entry in the priority list */
struct sched_entry *sched_head(struct scheduler *sched);
/* Removes the highest priority entry from the list*/
int sched_pop(struct scheduler *sched);

#endif
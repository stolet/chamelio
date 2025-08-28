#ifndef SCHED_H_
#define SCHED_H_

#include <stdint.h>

#include "queue.h"
#include "cham_fast.h"

/* Max number of entries in the scheduler */
#define MAX_SCHED_ENTRIES (128 * 1024)

/* Transmit scheduler that decides what should send next */
struct scheduler {
  /* Pre-allocated array of all entries in the priority list */
  struct cham_sched_entry entries[MAX_SCHED_ENTRIES];
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
int sched_add(struct scheduler *sched, struct cham_sched_entry *entry);
/* Removes an entry from the queue manager priority list */
int sched_remove(struct scheduler *sched, uint32_t id);
/* Peeks at the first entry in the priority list */
struct cham_sched_entry *sched_head(struct scheduler *sched);
/* Removes the highest priority entry from the list*/
int sched_pop(struct scheduler *sched);

#endif
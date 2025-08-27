#ifndef QMAN_H_
#define QMAN_H_

#include <stdint.h>

#include "queue.h"

/* Invalid ID used to represent end of list */
#define ID_INVALID (-1U)
/* Max number of entries in the queue manager */
#define MAX_QMENTRIES (128 * 1024)

/* Entry in the queue manager priority list */
struct qman_entry {
  /* Identifier for whatever is being scheduled (e.g. socket, flow) */
  uint32_t id;
  /* Next entry in the priority list */
  uint32_t next_entry;
  /* Priority for the entry */
  uint32_t priority;
  /* Units available for transmission */
  uint32_t avail;
};

struct qman {
  /* Pre-allocated array of all entries in the priority list */
  struct qman_entry entries[MAX_QMENTRIES];
  /* First element of the priority list */
  uint32_t head;
  /* Last element of the priority list */
  uint32_t tail;
  /* Head of free list entries */
  uint32_t free_head;
};

/* Initialises the queue manager for a protocol */
void qman_init(struct qman *qman);
/* Adds an entry to the queue manager priority list */
int qman_add(struct qman *qman, struct qman_entry *entry);
/* Removes an entry from the queue manager priority list */
int qman_remove(struct qman *qman, uint32_t id);
/* Removes the highest priority entry from the list*/
int qman_pop(struct qman *qman);

#endif
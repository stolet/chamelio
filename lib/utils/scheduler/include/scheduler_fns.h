#ifndef SCHED_H_
#define SCHED_H_

#include <linux/types.h>

#include "scheduler.h"

/* Initialises the scheduler for a protocol */
void sched_init(struct cham_scheduler *sched);
/* Peeks at the first entry in the priority list */
struct cham_sched_entry *sched_head(struct cham_scheduler *sched);
/* Removes the highest priority entry from the list*/
int sched_pop(struct cham_scheduler *sched);
/* Adds an entry to the queue manager priority list */
int sched_add(struct cham_scheduler *sched, __u32 id, __u32 priority,
    __u32 avail);

#endif

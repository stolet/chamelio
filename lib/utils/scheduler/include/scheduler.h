#ifndef SCHED_TYPES_H_
#define SCHED_TYPES_H_

#include <linux/types.h>

/* Max number of entries in the scheduler */
#define MAX_SCHED_ENTRIES (128 * 1024)
/* Invalid ID used to represent end of scheduler list */
#define SCHED_ID_INVALID (-1U)

/* Entry in the scheduler priority list */
struct cham_sched_entry {
  /* ID for the entry (e.g. flow_id or socket_id) */
  __u32 id;
  /* Next entry in the priority list */
  __u32 next_entry;
  /* Priority for the entry */
  __u32 priority;
  /* Units available for transmission */
  __u32 avail;
  /* Opaque pointer to a struct that wants to transmit (e.g. socket, flow) */
  __u64 opaque;
};

/* Transmit scheduler that decides what should send next */
struct cham_scheduler {
  /* Pre-allocated array of all entries in the priority list */
  struct cham_sched_entry entries[MAX_SCHED_ENTRIES];
  /* First element of the priority list */
  __u32 head;
  /* Last element of the priority list */
  __u32 tail;
};

#endif
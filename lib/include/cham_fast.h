#ifndef CHAM_FAST_H
#define CHAM_FAST_H

#include <stdint.h>

/* Max number of fast-path cores */
#define MAX_FP_CORES 16

/* Invalid ID used to represent end of list */
#define SCHED_ID_INVALID (-1U)

/* Ready entry for data staged before transmission */
struct cham_ready_entry {
  /* ID used by protocol to identify what should send */
  uint64_t id;
  /* How much data should be sent */
  uint32_t ready;
};

/* Entry in the scheduler priority list */
struct cham_sched_entry {
  /* Identifier for whatever is being scheduled (e.g. socket, flow) */
  uint64_t id;
  /* Next entry in the priority list */
  uint32_t next_entry;
  /* Priority for the entry */
  uint32_t priority;
  /* Units available for transmission */
  uint32_t avail;
};

#endif 
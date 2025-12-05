#ifndef TOMGR_H_
#define TOMGR_H_

#include <linux/types.h>

/* Max number of entries in timeout manager */
#define TOMGR_SIZE 64
#define TO_INVALID TOMGR_SIZE

enum to_type {
  TO_ARP = 0,
};

struct to_heap_entry {
  /* Index into entries array */
  int idx;
  /* Timeout used to sort the heap */
  __u64 to;
};

struct to_entry {
  /* Type of entry in the timeout manager */
  __u8 type;
  /* Timeout */
  __u64 to;
  /* Pointer to data associated with this timeout */
  void *data;
  /* Index of this entry in the heap */
  int heap_idx;
};

struct tomgr {
  /* Min-heap that stores indices into entries */
  struct to_heap_entry heap[TOMGR_SIZE];
  /* Array of entries */
  struct to_entry entries[TOMGR_SIZE];
  /* Current number of entries in the heap */
  int size;
};

/* Initializes the timeout manager */
struct tomgr * tomgr_init();
/* Inserts an entry into the timeout manager */
struct to_entry * tomgr_insert(struct tomgr *mgr, __u8 type, __u64 to, void *data);
/* Removes from timeout manager and cancels given entry */
int tomgr_cancel(struct tomgr *mgr, struct to_entry *e);
/* Looks at entry with earliest timeout without removing it */
struct to_entry * tomgr_peek(struct tomgr *mgr);
/* Removes and returns entry with earliest timeout */
struct to_entry * tomgr_pop(struct tomgr *mgr);

#endif
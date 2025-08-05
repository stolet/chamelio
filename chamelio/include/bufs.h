#ifndef BUFS_H_
#define BUFS_H_

#include <stdint.h>

struct cham_buf {
  /* Unique ID for the buffer */
  uint32_t id;
  /* Opaque pointer to the buffer struct in the library */
  uint64_t opaque;
  /* Base address of the buffer */
  uint64_t base;
  /* Length of the buffer */
  uint32_t len;
  /* Head of the buffer */
  uint64_t head;
  /* Number of bytes available in buffer */
  uint32_t avail;
  /* Slow path struct for the application this buffer belongs to.
     Only slow-path can touch this. */
  struct app_slow *app_slow;
  /* Fast path struct for the application this buffer belongs to.
     Only fast-path can touch this. */
  struct app_fast *app_fast;
};

/* Hash table containing buffers */
struct cham_buf_ht {
  /* Number of bins in hash table */
  uint32_t n_bins;
  /* TODO: Make a proper hash table with a hash and not just array index */
  /* Array containing the entries */
  struct cham_buf *entries;
};

#endif
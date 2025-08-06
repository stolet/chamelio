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
};

int bufs_init(struct cham_buf * buf);

#endif
#ifndef SHMALLOC_H_
#define SHMALLOC_H_

#include <linux/types.h>
#include <stddef.h>

/* Handle used to access a shared memory region */
struct shm_handle {
  /* A pointer into an allocated shared memory region */
  void *addr;
  /* The offset into the shared memory region from the base */
  __u64 off;
  /* Size of this allocation in bytes */
  size_t len;
  /* Next handle in list */
  struct shm_handle *next;
};

/* Allocator for a shared memory region */
struct shm_allocator {
  /* File descriptor of shared memory region */
  int shm_fd;
  /* Base pointer to mmapped region */
  void *shm_base;
  /* List of free memory */
  struct shm_handle *freelist;
};

struct shm_allocator * shmalloc_init(int shm_fd, void *shm_base, __u64 len);
int shmalloc_alloc(struct shm_allocator *alloc, size_t length, 
    struct shm_handle **handle);
void shmalloc_free(struct shm_allocator *alloc, struct shm_handle *handle);

#endif
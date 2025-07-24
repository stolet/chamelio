#ifndef SHMALLOC_H_
#define SHMALLOC_H_

#include <stdint.h>
#include <stddef.h>

struct shm_handle {
  uintptr_t base;
  size_t len;
  struct shm_handle *next;
};

struct shm_allocator {
  int shm_fd;
  void *shm_base;
  struct shm_handle *freelist;
};

struct shm_allocator * shmalloc_init(int shm_fd, void *shm_base, uint64_t len);
int shmalloc_alloc(struct shm_allocator *alloc, size_t length, 
    uintptr_t *off, struct shm_handle **handle);
void shmalloc_free(struct shm_allocator *alloc, struct shm_handle *handle);

#endif
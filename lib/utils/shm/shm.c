#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <numaif.h>
#include <sys/syscall.h>

#include "shm.h"
#include "log.h"

#define MAX_NODES 32
static int bind_numa(void *addr, size_t size, int numa_node);

void *shm_create_huge(const char *name, size_t size, 
    void *addr, int *fd, int numa_node)
{
  int ret;
  void *p;
  char path[128];

  snprintf(path, sizeof(path), "%s/%s", CHAMELIO_HUGE_PREFIX, name);

  *fd = open(path, O_CREAT | O_RDWR, 0666);
  if (*fd == -1) 
  {
    LOG_ERROR("open failed");
    perror("");
    goto error_out;
  }

  if (ftruncate(*fd, size) != 0) 
  {
    LOG_ERROR("ftruncate failed");
    perror("");
    goto error_remove;
  }

  p = mmap(addr, size, PROT_READ | PROT_WRITE,
      MAP_SHARED | (addr == NULL ? 0 : MAP_FIXED) | MAP_POPULATE, *fd, 0);
  if (p == (void *) -1)
  {
    LOG_ERROR("mmap failed");
    perror("");
    goto error_remove;
  }

  if (numa_node >= 0)
  {
    ret = bind_numa(p, size, numa_node);
    if (ret != 0)
      goto error_remove;
  }

  memset(p, 0, size);
  return p;

error_remove:
  close(*fd);
  shm_unlink(name);
error_out:
  return NULL;
}

void shm_destroy_huge(const char *name, size_t size, void *addr, int fd)
{
  char path[128];

  snprintf(path, sizeof(path), "%s/%s", CHAMELIO_HUGE_PREFIX, name);

  if (munmap(addr, size) != 0) 
  {
    LOG_WARN("munmap failed (%s)", strerror(errno));
  }
  unlink(path);
  close(fd);
}

static int bind_numa(void *addr, size_t size, int numa_node)
{
  unsigned long nmask[MAX_NODES];
  nmask[0] = 1UL << numa_node;
  if (mbind(addr, size, MPOL_BIND, nmask, 
      MAX_NODES, MPOL_MF_MOVE_ALL | MPOL_MF_STRICT) < 0)
  {
    LOG_ERROR("mbind failed");
    return -1;
  }

  return 0;
}

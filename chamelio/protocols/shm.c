#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

#include "shm.h"
#include "../../utils/log/log.h"

void *shm_create_huge(const char *name, size_t size, void *addr)
{
  int fd;
  void *p;
  char path[128];

  snprintf(path, sizeof(path), "%s/%s", CHAMELIO_HUGE_PREFIX, name);

  fd = open(path, O_CREAT | O_RDWR, 0666);
  if (fd == -1) 
  {
    LOG_ERROR("open failed");
    perror("");
    goto error_out;
  }

  if (ftruncate(fd, size) != 0) 
  {
    LOG_ERROR("ftruncate failed");
    perror("");
    goto error_remove;
  }

  p = mmap(addr, size, PROT_READ | PROT_WRITE,
      MAP_SHARED | (addr == NULL ? 0 : MAP_FIXED) | MAP_POPULATE, fd, 0);
  if (p == (void *) -1)
  {
    LOG_ERROR("mmap failed");
    perror("");
    goto error_remove;
  }

  memset(p, 0, size);

  close(fd);
  return p;

error_remove:
  close(fd);
  shm_unlink(name);
error_out:
  return NULL;
}

void shm_destroy_huge(const char *name, size_t size, void *addr)
{
  char path[128];

  snprintf(path, sizeof(path), "%s/%s", CHAMELIO_HUGE_PREFIX, name);

  if (munmap(addr, size) != 0) 
  {
    LOG_WARN("munmap failed (%s)", strerror(errno));
  }
  unlink(path);
}
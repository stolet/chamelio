#ifndef SHM_H_
#define SHM_H_

#include <stdlib.h>
#include <linux/types.h>

#define CHAMELIO_SHM_NAME "chamelio_shm"
#define CHAMELIO_SHM_NAME_INTERNAL "chamelio_internal"
#define CHAMELIO_HUGE_PREFIX "/dev/hugepages"

struct app_ctx {
  __u8 app_id;
  __u8 guest_id;

  /********************************************************/
  /* read-only fields */
  __u64 rx_base;
  __u64 tx_base;
  __u32 rx_len;
  __u32 tx_len;
  int	   evfd;

  /********************************************************/
  /* read-write fields */
  __u64 last_ts;
  __u32 rx_head;
  __u32 tx_head;
  __u32 rx_avail;
};

void *shm_create_huge(const char *name, size_t size, void *addr, int *fd,
    int numa_node);
void shm_destroy_huge(const char *name, size_t size, void *addr, int fd);

#endif

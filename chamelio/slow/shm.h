#ifndef SHM_H_
#define SHM_H_

#include <stdlib.h>
#include <stdint.h>

#define MAX_CORES 16
#define MAX_APPS 8
#define MAX_FLOWS (128 * 1024)

#define CHAMELIO_SHM_NAME "chamelio_shm"
#define CHAMELIO_SHM_INTERNAL "fp_state"
#define CHAMELIO_HUGE_PREFIX "/dev/hugepages"

struct app_ctx {
  uint8_t app_id;
  uint8_t guest_id;

  /********************************************************/
  /* read-only fields */
  uint64_t rx_base;
  uint64_t tx_base;
  uint32_t rx_len;
  uint32_t tx_len;
  int	   evfd;

  /********************************************************/
  /* read-write fields */
  uint64_t last_ts;
  uint32_t rx_head;
  uint32_t tx_head;
  uint32_t rx_avail;
};

void *shm_create_huge(const char *name, size_t size, void *addr, int *fd);
void shm_destroy_huge(const char *name, size_t size, void *addr, int fd);

#endif
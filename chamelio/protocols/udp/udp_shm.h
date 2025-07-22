#ifndef UDP_SHM_H
#define UDP_SHM_H_

#include <stdint.h>

#include "../shm.h"

struct udp_shm_internal {
  struct app_ctx appctx[MAX_CORES][MAX_APPS];
};

void * udp_init_shm(uint8_t id, char *name, uint64_t len);
void * udp_init_shm_internal(uint8_t id, char *name);

#endif
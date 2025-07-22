#include <stdlib.h>

#include "udp_shm.h"
#include "udp.h"
#include "../shm.h"
#include "../../config/config.h"
#include "../../../utils/utils.h"
#include "../../../utils/log/log.h"

void * udp_init_shm(uint8_t id, char *name, uint64_t len)
{
  void *shm_base;

  shm_base = shm_create_huge(name, len, NULL);
  if (shm_base == NULL)
  {
    LOG_ERROR("mapping shm failed");
    return NULL;
  }

  return shm_base;
}

void * udp_init_shm_internal(uint8_t id, char *name)
{
  void *shm_base;

  shm_base = shm_create_huge(name, sizeof(struct udp_shm_internal), NULL);
  if (shm_base == NULL)
  {
    LOG_ERROR("mapping shm internal failed");
    return NULL;
  }

  return shm_base;
}


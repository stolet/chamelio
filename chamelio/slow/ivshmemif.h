#ifndef IVSHMEMIF_H_
#define IVSHMEMIF_H_

#include <stdint.h>

#include "slow.h"

#define IVSHMEM_SOCKET_PATH "ivshmem_socket"

struct ivshmem_event {
  int type;
  int fd;
  uint8_t vmid;
};

int ivshmemif_init(struct slow_context *ctx);
int ivshmemif_poll(struct slow_context *ctx);

#endif
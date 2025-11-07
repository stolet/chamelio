#ifndef IVSHMEMIF_H_
#define IVSHMEMIF_H_

#include <linux/types.h>

#include "control.h"

#define IVSHMEM_SOCKET_PATH "ivshmem_socket"

struct ivshmem_event {
  int type;
  int fd;
  __u8 vmid;
};

int ivshmemif_init(struct control_context *ctx);
int ivshmemif_poll(struct control_context *ctx);

#endif
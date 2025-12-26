#ifndef IVSHMEMIF_H_
#define IVSHMEMIF_H_

#include <linux/types.h>

#include "control.h"

#define IVSHMEM_SOCKET_PATH "/run/chamelio/ivshmem_socket"

struct ivshmem_event {
  /* Type of ivshmem event */
  int type;
  /* File descriptor for the connection to the uxsocket */
  int fd;
  /* Guest protocol */
  struct guest_control *guest;
};

int ivshmemif_init(struct control_context *ctx);
int ivshmemif_poll(struct control_context *ctx);

#endif
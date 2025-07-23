#ifndef GUESTIF_H_
#define GUESTIF_H_

#include <stdint.h>

#include "slow.h"

#define GUEST_SOCKET_PATH "guest_socket"

struct guest_event {
  int type;
  int fd;
  uint8_t gid;
};

int guestif_init(struct slow_context *ctx);
int guestif_poll(struct slow_context *ctx);

#endif
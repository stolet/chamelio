#ifndef APPIF_H_
#define APPIF_H_

#include <stdint.h>

#include "control.h"
#include "queue.h"
#include "shmalloc.h"

/* This socket is only used by apps running on the
   bare machine to connect to Chamelio. They are 
   treated as a new guest and also assigned a new
   shared memory region. Regular guests use the
   guest socket to connect */
#define GUEST_SOCKET_PATH "guest_socket"

struct guest_event {
  int type;
  /* File descriptor for the connection to the uxsocket */
  int fd;
  /* Guest for protocol */
  struct guest_control *guest;
  /* Request for new protocol */
  struct queue_new_proto_req proto_req;
  /* Bytes received in uxsocket for proto request */
  size_t req_rx;
  /* Response for new protocol */
  struct queue_new_proto_res proto_res;
  /* Size of new protocol response */
  size_t resp_sz;
};

int guestif_init(struct control_context *ctx);
int guestif_poll(struct control_context *ctx);

#endif
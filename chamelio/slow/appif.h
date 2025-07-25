#ifndef APPIF_H_
#define APPIF_H_

#include <stdint.h>

#include "slow.h"
#include "queue.h"
#include "shmalloc.h"

/* This socket is only used by apps running on the
   bare machine to connect to Chamelio. They are 
   treated as a new guest and also assigned a new
   shared memory region. Regular guests use the
   guest socket to connect */
#define APP_SOCKET_PATH "app_socket"

struct app_event {
  int type;
  /* File descriptor for the connection to the uxsocket */
  int fd;
  /* Application ID */
  struct app_slow *app;
  /* Guest ID for this application */
  struct guest_slow *guest;
  /* Request for new application context */
  struct queue_new_app_ctx_req ctx_req;
  /* Bytes received in uxsocket for ctx request */
  size_t req_rx;
  /* Response to the application for a new app ctx */
  struct queue_new_app_ctx_res ctx_res;
  size_t resp_sz;
};

int appif_init(struct slow_context *ctx);
int appif_poll(struct slow_context *ctx);

#endif
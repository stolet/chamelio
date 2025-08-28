#ifndef APPIF_H_
#define APPIF_H_

#include <stdint.h>

#include "queue.h"
#include "udp_queue.h"
#include "udp_slow.h"

#define APP_SOCKET_PATH "app_socket"

struct app_event {
  int type;
  /* File descriptor for the connection to the uxsocket */
  int fd;
  /* Request for new app */
  struct udp_queue_new_actx_req app_req;
  /* Bytes received in uxsocket for app request */
  size_t req_rx;
  /* Response for new protocol */
  struct udp_queue_new_actx_res app_res;
  /* Size of new protocol response */
  size_t resp_sz;
  /* Application associated with this event */
  struct udp_app_slow *app;
};

int appif_init(struct udp_slow_context *ctx);
int appif_poll(struct udp_slow_context *ctx);

#endif
#ifndef APPIF_H_
#define APPIF_H_

#include <linux/types.h>

#include <cham_lib.h>

#include "queue.h"
#include "rpc_queue_types.h"
#include "rpc_slow.h"

struct app_event {
  int type;
  /* File descriptor for the connection to the uxsocket */
  int fd;
  /* Request for new app */
  struct rpc_queue_new_actx_req app_req;
  /* Bytes received in uxsocket for app request */
  size_t req_rx;
  /* Response for new protocol */
  struct rpc_queue_new_actx_res app_res;
  /* Size of new protocol response */
  size_t resp_sz;
  /* Application associated with this event */
  struct rpc_app_slow *app;
};

int appif_init(struct rpc_slow_context *ctx);
int appif_poll(struct rpc_slow_context *ctx);

#endif
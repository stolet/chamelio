#ifndef TCP_APPIF_H_
#define TCP_APPIF_H_

#include <linux/types.h>

#include <cham_lib.h>

#include "queue.h"
#include "tcp_queue_types.h"
#include "tcp_slow.h"

/*** Types ********************************************************************/

struct tcp_appif_event {
  int type;
  int fd;
  struct tcp_queue_new_actx_req app_req;
  size_t req_rx;
  struct tcp_queue_new_actx_res app_res;
  struct tcp_app_slow *app;
};

/*** Public API ***************************************************************/

int tcp_appif_init(struct tcp_slow_context *ctx);
int tcp_appif_poll(struct tcp_slow_context *ctx);

#endif

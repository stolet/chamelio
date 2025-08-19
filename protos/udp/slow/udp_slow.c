#include <stdlib.h>
#include <cham_lib.h>

#include "appif.h"
#include "udp_slow.h"
#include "log.h"

int init_udp_slow_context(struct udp_slow_context *ctx);

int main(int argc, char **argv)
{
  int ret;
  struct udp_slow_context ctx;
  
  ret = init_udp_slow_context(&ctx);
  if (ret != 0)
  {
    LOG_ERROR("failed to initialise udp slow context");
    abort();
  }
  
  ret = appif_init(&ctx);
  if (ret != 0)
  {
    LOG_ERROR("failed to initialise appiff");
    abort();
  }
  
  while (1)
  {
    appif_poll(&ctx);
  }
}

int init_udp_slow_context(struct udp_slow_context *ctx)
{
  struct guest_lib *g;
  struct proto_lib *p;

  g = cham_connect_guest();
  if (g == NULL)
  {
    LOG_ERROR("UDP slow-path couldn't connect to Chamelio");
    abort();
  }

  p = cham_new_proto(g, 0);
  if (p == NULL)
  {
    LOG_ERROR("UDP slow-path failed to register protocol with Chamelio");
    abort();
  }

  ctx->app_uxfd = -1;
  ctx->app_epfd = -1;
  ctx->guest = g;
  ctx->proto = p;
  ctx->n_apps = 0;

  return 0;
}

int poll_apps(struct udp_slow_context *ctx)
{
  return 0;
}

int poll_control(struct udp_slow_context *ctx)
{
  return 0;
}
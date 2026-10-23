#include <stdlib.h>

#include "homa_slow.h"
#include "homa_config.h"
#include "log.h"

int main(int argc, char **argv)
{
  int ret;
  struct homa_slow ctx;

  ret = homa_config_parse(&ctx.config, argc, argv);
  if (ret != 0)
  {
    LOG_ERROR("failed to parse Homa configuration");
    abort();
  }

  ret = homa_init_context(&ctx);
  if (ret != 0)
  {
    LOG_ERROR("failed to initialise homa slow context");
    abort();
  }

  ret = homa_app_init(&ctx);
  if (ret != 0)
  {
    LOG_ERROR("failed to initialise appif");
    abort();
  }

  while (1)
  {
    homa_timer_poll(&ctx);
    homa_app_poll(&ctx);
  }
}

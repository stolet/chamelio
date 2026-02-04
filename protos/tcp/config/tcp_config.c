#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tcp_config.h"

enum cfg_params {
  CP_VIRT,
};

static struct option opts[] = {
  { .name = "virt",
    .has_arg = no_argument,
    .val = CP_VIRT },
};

static int config_defaults(struct tcp_configuration *c);
static void print_usage(char *progname);

int tcp_config_parse(struct tcp_configuration *c, int argc, char **argv)
{
  int ret, done = 0;

  ret = config_defaults(c);
  if (ret != 0)
  {
    fprintf(stderr, "config_defaults failed\n");
    goto failed;
  }

  while (!done)
  {
    ret = getopt_long(argc, argv, "", opts, NULL);
    switch (ret)
    {
      case CP_VIRT:
        c->virt = 1;
        break;
      case -1:
        done = 1;
        break;
      case '?':
        goto failed;
      default:
        abort();
    }
  }

  return 0;

failed:
  print_usage(argv[0]);
  return -1;
}

static int config_defaults(struct tcp_configuration *c)
{
  c->virt = 0;
  return 0;
}

static void print_usage(char *progname)
{
  fprintf(stderr, "Usage: %s [OPTION]...\n"
      "\n"
      "Virtualization:\n"
      "  --virt                              Enable virtualization features"
          "[default: disabled]\n"
      "\n"
      , progname);
}

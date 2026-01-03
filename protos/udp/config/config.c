#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "udp_config.h"

enum cfg_params {
  CP_VIRT_OFF,
};

static struct option opts[] = {
  { .name = "virt-off",
    .has_arg = no_argument,
    .val = CP_VIRT_OFF },
};

static int config_defaults(struct udp_configuration *c);
static void print_usage(char *progname);

int udp_config_parse(struct udp_configuration *c, int argc, char **argv)
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
      case CP_VIRT_OFF:
        c->virt = 0;
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

static int config_defaults(struct udp_configuration *c)
{
  c->virt = 1;
  return 0;
}

static void print_usage(char *progname)
{
  fprintf(stderr, "Usage: %s [OPTION]...\n"
      "\n"
      "Virtualization:\n"
      "  --virt-off                              Disable virtualization features"
          "[default: enabled]\n"
      "\n"
      , progname);
}

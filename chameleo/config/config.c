#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "config.h"
#include "../../utils/log/log.h"

enum cfg_params {
  CP_FP_CORES_MAX,
  CP_FP_NO_XSUMOFFLOAD,
  CP_DPDK_EXTRA,
};

static struct option opts[] = {
  { .name = "fp-cores-max",
    .has_arg = required_argument,
    .val = CP_FP_CORES_MAX },
  {
    .name = "fp-no-xsumoffload",
    .has_arg = no_argument,
    .val = CP_FP_NO_XSUMOFFLOAD },
  { .name = "dpdk-extra",
    .has_arg = required_argument,
    .val = CP_DPDK_EXTRA },
};

static int config_defaults(struct configuration *c, char *progname);
static void print_usage(struct configuration *c, char *progname);
static inline int parse_int32(const char *s, uint32_t *pu32);
static inline int parse_arg_append(char *s, struct configuration *c);

int config_parse(struct configuration *c, int argc, char **argv)
{
  int ret, done = 0;

  ret = config_defaults(c, argv[0]);
  if (ret != 0)
  {
    LOG_ERROR("config_defaults failed");
    goto failed;
  }

  while (!done)
  {
    ret = getopt_long(argc, argv, "", opts, NULL);
    switch (ret)
    {
      case CP_FP_CORES_MAX:
        if (parse_int32(optarg, &c->fp_cores_max) != 0) {
          LOG_ERROR("fp cores max parsing failed");
          goto failed;
        }
        break;
      case CP_FP_NO_XSUMOFFLOAD:
        c->fp_xsumoffloads = 0;
        break;
      case CP_DPDK_EXTRA:
        if (parse_arg_append(optarg, c) != 0) {
          goto failed;
        }
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
  print_usage(c, argv[0]);
  return -1;
}

static int config_defaults(struct configuration *c, char *progname)
{
  c->fp_cores_max = 1;
  c->fp_xsumoffloads = 1;

  c->dpdk_argc = 1;
  if ((c->dpdk_argv = calloc(2, sizeof(*c->dpdk_argv))) == NULL) {
    LOG_ERROR("calloc failed");
    return -1;
  }
  c->dpdk_argv[0] = progname;

  return 0;
}

static void print_usage(struct configuration *c, char *progname)
{
  fprintf(stderr, "Usage: %s [OPTION]... \n", progname);
}

static inline int parse_int32(const char *s, uint32_t *pi)
{
  char *end;
  *pi = strtoul(s, &end, 10);
  if (!*s || *end)
    return -1;
  return 0;
}

static inline int parse_arg_append(char *s, struct configuration *c)
{
  char **new;

  if ((new = realloc(c->dpdk_argv, sizeof(char *) * (c->dpdk_argc + 2)))
      == NULL)
  {
    LOG_ERROR("alloc failed");
    return -1;
  }

  new[c->dpdk_argc++] = strdup(s);
  c->dpdk_argv = new;

  return 0;
}
#include <getopt.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "homa_config.h"
#include "homa.h"

enum cfg_params {
  CP_VIRT,
  CP_UNSCHEDULED,
  CP_GRANT,
  CP_INCOMING,
};

static struct option opts[] = {
  { .name = "virt",
    .has_arg = no_argument,
    .val = CP_VIRT },
  { .name = "unscheduled-bytes",
    .has_arg = required_argument,
    .val = CP_UNSCHEDULED },
  { .name = "grant-bytes",
    .has_arg = required_argument,
    .val = CP_GRANT },
  { .name = "incoming-bytes",
    .has_arg = required_argument,
    .val = CP_INCOMING },
  { .name = NULL },
};

static int config_defaults(struct homa_configuration *c);
static void print_usage(char *progname);
static int parse_bytes(const char *text, __u32 *value, __u32 limit);

int homa_config_parse(struct homa_configuration *c, int argc, char **argv)
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
      case CP_UNSCHEDULED:
        if (parse_bytes(optarg, &c->unscheduled_bytes,
            HOMA_MESSAGE_BYTES) != 0)
          goto failed;
        break;
      case CP_GRANT:
        if (parse_bytes(optarg, &c->grant_bytes, HOMA_MESSAGE_BYTES) != 0)
          goto failed;
        break;
      case CP_INCOMING:
        if (parse_bytes(optarg, &c->incoming_bytes,
            HOMA_MAX_RPCS * HOMA_MESSAGE_BYTES) != 0)
          goto failed;
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

  if (optind != argc)
  {
    fprintf(stderr, "unexpected argument: %s\n", argv[optind]);
    goto failed;
  }

  return 0;

failed:
  print_usage(argv[0]);
  return -1;
}

static int config_defaults(struct homa_configuration *c)
{
  c->virt = 0;
  c->unscheduled_bytes = HOMA_UNSCHEDULED_BYTES;
  c->grant_bytes = HOMA_GRANT_BYTES;
  c->incoming_bytes = HOMA_INCOMING_BYTES;
  return 0;
}

static int parse_bytes(const char *text, __u32 *value, __u32 limit)
{
  unsigned long parsed;
  char *end;

  errno = 0;
  parsed = strtoul(text, &end, 10);
  if (errno != 0 || *text == '\0' || *text == '-' || *end != '\0' ||
      parsed < HOMA_MAX_PAYLOAD || parsed > limit)
    return -1;

  *value = parsed;
  return 0;
}

static void print_usage(char *progname)
{
  fprintf(stderr, "Usage: %s [OPTION]...\n"
      "\n"
      "Virtualization:\n"
      "  --virt                              Enable virtualization features"
          " [default: disabled]\n"
      "\n"
      "Receiver windows (bytes, rounded down to complete DATA segments):\n"
      "  --unscheduled-bytes N                Initial allowance [16384]\n"
      "  --grant-bytes N                      Grant window [16384]\n"
      "  --incoming-bytes N                   Incoming budget [65536]\n"
      "\n"
      , progname);
}

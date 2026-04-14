#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "udp_config.h"

enum cfg_params {
  CP_VIRT,
  CP_RXBUF_SZ,
  CP_TXBUF_SZ,
  CP_APPQ_LEN,
  CP_BUMPQ_LEN,
  CP_CTLQ_LEN,
};

static struct option opts[] = {
  { .name = "virt",
    .has_arg = no_argument,
    .val = CP_VIRT },
  { .name = "rxbuf-sz",
    .has_arg = required_argument,
    .val = CP_RXBUF_SZ },
  { .name = "txbuf-sz",
    .has_arg = required_argument,
    .val = CP_TXBUF_SZ },
  { .name = "appq-len",
    .has_arg = required_argument,
    .val = CP_APPQ_LEN },
  { .name = "bumpq-len",
    .has_arg = required_argument,
    .val = CP_BUMPQ_LEN },
  { .name = "ctlq-len",
    .has_arg = required_argument,
    .val = CP_CTLQ_LEN },
};

static int config_defaults(struct udp_configuration *c);
static int parse_u32(const char *s, __u32 *val);
static void print_usage(struct udp_configuration *c, char *progname);

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
      case CP_VIRT:
        c->virt = 1;
        break;
      case CP_RXBUF_SZ:
        if (parse_u32(optarg, &c->rxbuf_sz) != 0)
          goto failed;
        break;
      case CP_TXBUF_SZ:
        if (parse_u32(optarg, &c->txbuf_sz) != 0)
          goto failed;
        break;
      case CP_APPQ_LEN:
        if (parse_u32(optarg, &c->appq_len) != 0)
          goto failed;
        break;
      case CP_BUMPQ_LEN:
        if (parse_u32(optarg, &c->bumpq_len) != 0)
          goto failed;
        break;
      case CP_CTLQ_LEN:
        if (parse_u32(optarg, &c->ctlq_len) != 0)
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

  return 0;

failed:
  print_usage(c, argv[0]);
  return -1;
}

static int config_defaults(struct udp_configuration *c)
{
  c->virt = 0;
  c->virt_gre = 0;
  c->rxbuf_sz = 32768;
  c->txbuf_sz = 32768;
  c->appq_len = 128;
  c->bumpq_len = 65536;
  c->ctlq_len = 1024;
  return 0;
}

static int parse_u32(const char *s, __u32 *val)
{
  char *end;
  unsigned long parsed;

  errno = 0;
  parsed = strtoul(s, &end, 0);
  if (errno != 0 || *s == '\0' || *end != '\0' || parsed == 0 ||
      parsed > UINT_MAX)
  {
    fprintf(stderr, "invalid value: %s\n", s);
    return -1;
  }

  *val = (__u32) parsed;
  return 0;
}

static void print_usage(struct udp_configuration *c, char *progname)
{
  fprintf(stderr, "Usage: %s [OPTION]...\n"
      "\n"
      "Memory size:\n"
      "  --rxbuf-sz=BYTES                       RX buffer size in bytes"
          " [default: %u]\n"
      "  --txbuf-sz=BYTES                       TX buffer size in bytes"
          " [default: %u]\n"
      "  --appq-len=NELEMS                      App queue length in elements"
          " [default: %u]\n"
      "  --bumpq-len=NELEMS                     Bump queue length in elements"
          " [default: %u]\n"
      "  --ctlq-len=NELEMS                     Control queue length in elements"
          " [default: %u]\n"
      "\n"
      "Virtualization:\n"
      "  --virt                              Enable virtualization features"
          "[default: disabled]\n"
      "\n"
      , progname, c->rxbuf_sz, c->txbuf_sz, c->appq_len, c->bumpq_len,
      c->ctlq_len);
}

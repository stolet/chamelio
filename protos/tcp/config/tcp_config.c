#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tcp_config.h"
#include "log.h"

enum cfg_params {
  CP_VIRT,
  CP_RXBUF_SZ,
  CP_TXBUF_SZ,
  CP_APPQ_LEN,
  CP_BUMPQ_LEN,
  CP_CTLQ_LEN,
  CP_TCP_RTT_INIT,
  CP_CC,
  CP_CC_CONTROL_GRANULARITY,
  CP_CC_CONTROL_INTERVAL,
  CP_CC_REMIT_INTS,
  CP_CC_DCTCP_WEIGHT,
  CP_CC_DCTCP_INIT,
  CP_CC_DCTCP_STEP,
  CP_CC_DCTCP_MIMD,
  CP_CC_DCTCP_MIN,
  CP_CC_DCTCP_MINPKTS,
  CP_CC_CONST_RATE,
  CP_DEBUG,
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
  { .name = "cc-rtt-init",
    .has_arg = required_argument,
    .val = CP_TCP_RTT_INIT },
  { .name = "cc",
    .has_arg = required_argument,
    .val = CP_CC },
  { .name = "cc-control-granularity",
    .has_arg = required_argument,
    .val = CP_CC_CONTROL_GRANULARITY },
  { .name = "cc-control-interval",
    .has_arg = required_argument,
    .val = CP_CC_CONTROL_INTERVAL },
  { .name = "cc-remit-ints",
    .has_arg = required_argument,
    .val = CP_CC_REMIT_INTS },
  { .name = "cc-dctcp-weight",
    .has_arg = required_argument,
    .val = CP_CC_DCTCP_WEIGHT },
  { .name = "cc-dctcp-init",
    .has_arg = required_argument,
    .val = CP_CC_DCTCP_INIT },
  { .name = "cc-dctcp-step",
    .has_arg = required_argument,
    .val = CP_CC_DCTCP_STEP },
  { .name = "cc-dctcp-mimd",
    .has_arg = required_argument,
    .val = CP_CC_DCTCP_MIMD },
  { .name = "cc-dctcp-min",
    .has_arg = required_argument,
    .val = CP_CC_DCTCP_MIN },
  { .name = "cc-dctcp-minpkts",
    .has_arg = required_argument,
    .val = CP_CC_DCTCP_MINPKTS },
  { .name = "cc-const-rate",
    .has_arg = required_argument,
    .val = CP_CC_CONST_RATE },
  { .name = "debug",
    .has_arg = no_argument,
    .val = CP_DEBUG },
  { .name = NULL },
};

static int config_defaults(struct tcp_configuration *c);
static int parse_u32(const char *s, __u32 *val);
static int parse_u32_zero(const char *s, __u32 *val);
static int parse_double(const char *s, double *val);
static int parse_cc_algorithm(const char *s, __u32 *val);
static void print_usage(struct tcp_configuration *c, char *progname);

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
      case CP_TCP_RTT_INIT:
        if (parse_u32(optarg, &c->cc_rtt_init) != 0)
          goto failed;
        break;
      case CP_CC:
        if (parse_cc_algorithm(optarg, &c->cc_algorithm) != 0)
          goto failed;
        break;
      case CP_CC_CONTROL_GRANULARITY:
        if (parse_u32(optarg, &c->cc_control_granularity) != 0)
          goto failed;
        break;
      case CP_CC_CONTROL_INTERVAL:
        if (parse_u32(optarg, &c->cc_control_interval) != 0)
          goto failed;
        break;
      case CP_CC_REMIT_INTS:
        if (parse_u32(optarg, &c->cc_remit_ints) != 0)
          goto failed;
        break;
      case CP_CC_DCTCP_WEIGHT: {
        double d;

        if (parse_double(optarg, &d) != 0 || d < 0 || d > 1)
          goto failed;
        c->cc_dctcp_weight = UINT_MAX * d;
        break;
      }
      case CP_CC_DCTCP_INIT:
        if (parse_u32(optarg, &c->cc_dctcp_init) != 0)
          goto failed;
        break;
      case CP_CC_DCTCP_STEP:
        if (parse_u32(optarg, &c->cc_dctcp_step) != 0)
          goto failed;
        break;
      case CP_CC_DCTCP_MIMD: {
        double d;

        if (parse_double(optarg, &d) != 0 || d < 1 || d > 2)
          goto failed;
        c->cc_dctcp_mimd = UINT_MAX * (d - 1);
        break;
      }
      case CP_CC_DCTCP_MIN:
        if (parse_u32_zero(optarg, &c->cc_dctcp_min) != 0)
          goto failed;
        break;
      case CP_CC_DCTCP_MINPKTS:
        if (parse_u32(optarg, &c->cc_dctcp_minpkts) != 0)
          goto failed;
        break;
      case CP_CC_CONST_RATE:
        if (parse_u32_zero(optarg, &c->cc_const_rate) != 0)
          goto failed;
        break;
      case CP_DEBUG:
        c->debug = 1;
        log_set_level(CHAM_LOG_LEVEL_DEBUG);
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

static int config_defaults(struct tcp_configuration *c)
{
  c->virt = 0;
  c->virt_gre = 0;
  c->rxbuf_sz = 32768;
  c->txbuf_sz = 32768;
  c->appq_len = 128;
  c->bumpq_len = 65536;
  c->ctlq_len = 1024;
  c->cc_rtt_init = 50;
  c->cc_algorithm = TCP_CC_ALGO_CONST_RATE;
  c->cc_control_granularity = 50;
  c->cc_control_interval = 2;
  c->cc_remit_ints = 4;
  c->cc_dctcp_weight = UINT_MAX / 16;
  c->cc_dctcp_init = 10000;
  c->cc_dctcp_step = 10000;
  c->cc_dctcp_mimd = 0;
  c->cc_dctcp_min = 0;
  c->cc_dctcp_minpkts = 50;
  c->cc_const_rate = 0;
  c->debug = 0;
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

static int parse_u32_zero(const char *s, __u32 *val)
{
  char *end;
  unsigned long parsed;

  errno = 0;
  parsed = strtoul(s, &end, 0);
  if (errno != 0 || *s == '\0' || *end != '\0' || parsed > UINT_MAX)
  {
    fprintf(stderr, "invalid value: %s\n", s);
    return -1;
  }

  *val = (__u32) parsed;
  return 0;
}

static int parse_double(const char *s, double *val)
{
  char *end;

  errno = 0;
  *val = strtod(s, &end);
  if (errno != 0 || *s == '\0' || *end != '\0')
  {
    fprintf(stderr, "invalid value: %s\n", s);
    return -1;
  }

  return 0;
}

static int parse_cc_algorithm(const char *s, __u32 *val)
{
  if (strcmp(s, "const-rate") == 0)
  {
    *val = TCP_CC_ALGO_CONST_RATE;
    return 0;
  }

  if (strcmp(s, "dctcp-rate") == 0)
  {
    *val = TCP_CC_ALGO_DCTCP_RATE;
    return 0;
  }

  fprintf(stderr, "invalid congestion-control algorithm: %s\n", s);
  return -1;
}

static void print_usage(struct tcp_configuration *c, char *progname)
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
      "Congestion Control:\n"
      "  --cc=ALGORITHM                        Congestion-control algorithm"
          " [default: %s]\n"
      "     Options: const-rate, dctcp-rate\n"
      "  --cc-rtt-init=USEC                   Initial RTT estimate in microseconds"
          " [default: %u]\n"
      "  --cc-control-granularity=USEC        Minimum control-loop interval"
          " [default: %u]\n"
      "  --cc-control-interval=RTTS           Control interval in RTTs"
          " [default: %u]\n"
      "  --cc-remit-ints=INTERVALS            Control intervals without ACKs"
          " before retransmit [default: %u]\n"
      "  --cc-dctcp-weight=WEIGHT             DCTCP EWMA weight in [0, 1]"
          " [default: %f]\n"
      "  --cc-dctcp-init=KBPS                 DCTCP initial rate in kbps"
          " [default: %u]\n"
      "  --cc-dctcp-step=KBPS                 DCTCP additive increase step in kbps"
          " [default: %u]\n"
      "  --cc-dctcp-mimd=FACTOR               DCTCP multiplicative increase factor"
          " in [1, 2] [default: disabled]\n"
      "  --cc-dctcp-min=KBPS                  DCTCP minimum rate in kbps"
          " [default: %u]\n"
      "  --cc-dctcp-minpkts=ACKS              DCTCP minimum ACKs per sample"
          " [default: %u]\n"
      "  --cc-const-rate=KBPS                 Constant rate in kbps, 0 means"
          " unlimited [default: %u]\n"
      "\n"
      , progname, c->rxbuf_sz, c->txbuf_sz, c->appq_len, c->bumpq_len,
      c->ctlq_len,
      c->cc_algorithm == TCP_CC_ALGO_DCTCP_RATE ? "dctcp-rate" : "const-rate",
      c->cc_rtt_init, c->cc_control_granularity, c->cc_control_interval,
      c->cc_remit_ints, (double) c->cc_dctcp_weight / UINT_MAX,
      c->cc_dctcp_init, c->cc_dctcp_step, c->cc_dctcp_min,
      c->cc_dctcp_minpkts, c->cc_const_rate);
}

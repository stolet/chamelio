#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <arpa/inet.h>
#include <inttypes.h>

#include "config.h"
#include "log.h"

enum cfg_params {
  CP_SHM_LEN,
  CP_SHM_INTERNAL_LEN,
  CP_CHAM_QUEUE_LEN,
  CP_APP_QUEUE_LEN,
  CP_APP_CTX_RX_QUEUE_LEN,
  CP_APP_CTX_TX_QUEUE_LEN,
  CP_AGT_QUEUE_LEN,
  CP_RXBUF_LEN,
  CP_TXBUF_LEN,
  CP_IP_ADDR,
  CP_IP_ROUTE,
  CP_MAX_GUESTS,
  CP_MAX_APPS,
  CP_MAX_APP_CTXS,
  CP_FP_CORES_MAX,
  CP_FP_NO_XSUMOFFLOAD,
  CP_DPDK_EXTRA,
};

static struct option opts[] = {
  { .name = "shm-len",
    .has_arg = required_argument,
    .val = CP_SHM_LEN },
  { .name = "shm-internal-len",
    .has_arg = required_argument,
    .val = CP_SHM_INTERNAL_LEN },
  { .name = "cham-queue-len",
    .has_arg = required_argument,
    .val = CP_CHAM_QUEUE_LEN },
  { .name = "app-queue-len",
    .has_arg = required_argument,
    .val = CP_APP_QUEUE_LEN },
  { .name = "app-ctx-rx-queue-len",
    .has_arg = required_argument,
    .val = CP_APP_CTX_RX_QUEUE_LEN },
  { .name = "app-ctx-tx-queue-len",
    .has_arg = required_argument,
    .val = CP_APP_CTX_TX_QUEUE_LEN },
  { .name = "agt-queue-len",
    .has_arg = required_argument,
    .val = CP_AGT_QUEUE_LEN },
  { .name = "rxbuf-len",
    .has_arg = required_argument,
    .val = CP_RXBUF_LEN },
  { .name = "txbuf-len",
    .has_arg = required_argument,
    .val = CP_TXBUF_LEN },
  { .name = "ip-addr",
    .has_arg = required_argument,
    .val = CP_IP_ADDR },
  { .name = "ip-route",
    .has_arg = required_argument,
    .val = CP_IP_ROUTE },
  { .name = "max-guests",
    .has_arg = required_argument,
    .val = CP_MAX_GUESTS },
  { .name = "max-apps",
    .has_arg = required_argument,
    .val = CP_MAX_APPS },
  { .name = "max-app-ctxs",
    .has_arg = required_argument,
    .val = CP_MAX_APP_CTXS },
  { .name = "fp-cores-max",
    .has_arg = required_argument,
    .val = CP_FP_CORES_MAX },
  { .name = "fp-no-xsumoffload",
    .has_arg = no_argument,
    .val = CP_FP_NO_XSUMOFFLOAD },
  { .name = "dpdk-extra",
    .has_arg = required_argument,
    .val = CP_DPDK_EXTRA },
};

static int config_defaults(struct configuration *c, char *progname);
static void print_usage(struct configuration *c, char *progname);
static int parse_int64(const char *s, uint64_t *pi);
static int parse_int32(const char *s, uint32_t *pu32);
static int parse_arg_append(char *s, struct configuration *c);
static int parse_cidr(char *s, uint32_t *ip, uint8_t *prefix);
static int parse_route(char *s, struct configuration *c);
static int parse_ipv4(const char *s, uint32_t *ip);

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
      case CP_SHM_LEN:
        if (parse_int64(optarg, &c->shm_len) != 0) {
          fprintf(stderr, "shm len parsing failed\n");
          goto failed;
        }
        break;
      case CP_SHM_INTERNAL_LEN:
        if (parse_int64(optarg, &c->shm_internal_len) != 0) {
          fprintf(stderr, "shm internal len parsing failed\n");
          goto failed;
        }
        break;
      case CP_CHAM_QUEUE_LEN:
        if (parse_int64(optarg, &c->cham_queue_len) != 0) {
          fprintf(stderr, "chamelio queue len parsing failed\n");
          goto failed;
        }
        break;
      case CP_APP_QUEUE_LEN:
        if (parse_int64(optarg, &c->app_queue_len) != 0) {
          fprintf(stderr, "app queue len parsing failed\n");
          goto failed;
        }
        break;
      case CP_APP_CTX_RX_QUEUE_LEN:
        if (parse_int64(optarg, &c->app_ctx_rx_queue_len) != 0) {
          fprintf(stderr, "app ctx rx queue len parsing failed\n");
          goto failed;
        }
        break;
      case CP_APP_CTX_TX_QUEUE_LEN:
        if (parse_int64(optarg, &c->app_ctx_tx_queue_len) != 0) {
          fprintf(stderr, "app ctx tx queue len parsing failed\n");
          goto failed;
        }
        break;
      case CP_AGT_QUEUE_LEN:
        if (parse_int64(optarg, &c->agt_queue_len) != 0) {
          fprintf(stderr, "agent queue len parsing failed\n");
          goto failed;
        }
        break;
      case CP_RXBUF_LEN:
        if (parse_int64(optarg, &c->rxbuf_len) != 0) {
          fprintf(stderr, "rx buffer len parsing failed\n");
          goto failed;
        }
        break;
      case CP_TXBUF_LEN:
        if (parse_int64(optarg, &c->txbuf_len) != 0) {
          fprintf(stderr, "tx buffer len parsing failed\n");
          goto failed;
        }
        break;
      case CP_IP_ADDR:
        c->ip_prefix = 0;
        if (parse_cidr(optarg, &c->ip, &c->ip_prefix) != 0) {
          fprintf(stderr, "Parsing IP failed\n");
          goto failed;
        }
        break;
      case CP_IP_ROUTE:
        if (parse_route(optarg, c) != 0) {
          goto failed;
        }
        break;
      case CP_MAX_GUESTS:
        if (parse_int32(optarg, &c->max_guests) != 0) {
          LOG_ERROR("max guests parsing failed");
          goto failed;
        }
        break;
      case CP_MAX_APPS:
        if (parse_int32(optarg, &c->max_apps) != 0) {
          LOG_ERROR("max apps parsing failed");
          goto failed;
        }
        break;
      case CP_MAX_APP_CTXS:
        if (parse_int32(optarg, &c->max_app_ctxs) != 0) {
          LOG_ERROR("max apps ctxs parsing failed");
          goto failed;
        }
        break;
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
  c->shm_len = 1024 * 1024 * 1024;
  c->shm_internal_len = 1024 * 1024 * 32;
  c->cham_queue_len = 16 * 1024;
  c->app_queue_len = 1024 * 1024;
  c->app_ctx_rx_queue_len = 64 * 32 * 1024;
  c->app_ctx_tx_queue_len = 64 * 8192;
  c->agt_queue_len = 16 * 1024;
  c->rxbuf_len = 8192;
  c->txbuf_len = 8192;
  c->ip = 0;
  c->max_guests = 128;
  c->max_apps = 32;
  c->max_app_ctxs = 32;
  c->fp_cores_max = 1;
  c->fp_xsumoffloads = 1;

  c->dpdk_argc = 1;
  if ((c->dpdk_argv = calloc(2, sizeof(*c->dpdk_argv))) == NULL) 
  {
    LOG_ERROR("calloc failed");
    return -1;
  }
  c->dpdk_argv[0] = progname;

  return 0;
}

static void print_usage(struct configuration *c, char *progname)
{
  fprintf(stderr, "Usage: %s [OPTION]... --ip-addr=IP[/PREFIXLEN]\n"
      "\n"
      "Memory sizes:\n"
      "  --shm-len=LEN                           Shared memory len"
           "[default: %"PRIu64"]\n"
      "  --shm-internal-len=LEN                  Internal shared memory len"
           "[default: %"PRIu64"]\n"
      "  --cham-queue-len=LEN                    Chamelio Fast <-> Slow queue len"
           "[default: %"PRIu64"]\n"
      "  --app-queue-len=LEN                     Application <-> Chamelio queue len"
           "[default: %"PRIu64"]\n"
      "  --app-ctx-rx-queue-len=LEN              RX bump messages to app context queue len"
           "[default: %"PRIu64"]\n"
      "  --app-ctx-tx-queue-len=LEN              TX bump messages to app context queue len"
           "[default: %"PRIu64"]\n"
      "  --agt-queue-len=LEN                     Guest agent <-> Chamelio queue len"
           "[default: %"PRIu64"]\n"
      "  --rxbuf-len=LEN                         Application RX buffer len"
           "[default: %"PRIu64"]\n"
      "  --txbuf-len=LEN                         Application TX buffer len len"
           "[default: %"PRIu64"]\n"
      "IP protocol parameters:\n"
      "  --ip-route=DEST[/PREFIX],NEXTHOP        Add route\n"
      "  --ip-addr=ADDR[/PREFIXLEN]              Set local IP address\n"
      "Max values:\n"
      "  --max-guests=GUESTS                     Max number of guests\n"
          "[default: %"PRIu32"]\n"
      "  --max-apps=APPS                         Max number of apps per guest\n"
          "[default: %"PRIu32"]\n"
      "  --max-app-ctxs=CTXS                     Max number of app ctxs per guest\n"
          "[default: %"PRIu32"]\n"
      "Fast path:\n"
      "  --fp-cores-max=CORES                    Max cores used for fast path"
           "[default: %"PRIu32"]\n"
      "  --fp-no-xsumoffload                     Disable TX Checksum offload "
          "[default: enabled]\n"
      "Miscelaneous:\n"
      "  --dpdk-extra=ARG                        Add extra DPDK argument"
      "\n"
      ,progname, 
      c->shm_len, c->shm_internal_len,
      c->cham_queue_len, c->app_queue_len, 
      c->app_ctx_rx_queue_len, c->app_ctx_tx_queue_len,
      c->agt_queue_len,
      c->rxbuf_len, c->txbuf_len,
      c->max_guests, c->max_apps, c->max_app_ctxs,
      c->fp_cores_max);
}

static int parse_int64(const char *s, uint64_t *pi)
{
  char *end;
  *pi = strtoul(s, &end, 10);
  if (!*s || *end)
    return -1;
  return 0;
}

static int parse_int32(const char *s, uint32_t *pi)
{
  char *end;
  *pi = strtoul(s, &end, 10);
  if (!*s || *end)
    return -1;
  return 0;
}

static int parse_int8(const char *s, uint8_t *pi)
{
  char *end;
  *pi = strtoul(s, &end, 10);
  if (!*s || *end)
    return -1;
  return 0;
}

static int parse_arg_append(char *s, struct configuration *c)
{
  char **new;

  new = realloc(c->dpdk_argv, sizeof(char *) * (c->dpdk_argc + 2));
  if (new == NULL)
  {
    LOG_ERROR("alloc failed");
    return -1;
  }

  new[c->dpdk_argc++] = strdup(s);
  c->dpdk_argv = new;

  return 0;
}

static int parse_cidr(char *s, uint32_t *ip, uint8_t *prefix)
{
  char *slash;

  /* parse /prefix and replace / by \0 (if applicable)*/
  if ((slash = strrchr(s, '/')) != NULL) 
  {
    if (parse_int8(slash + 1, prefix) != 0) 
    {
      LOG_ERROR("parsing prefix (%s) failed", slash);
      return -1;
    }
    *slash = 0;
  }

  if (parse_ipv4(s, ip) != 0) 
  {
    LOG_ERROR("parsing IP (%s) failed", s);
    return -1;
  }

  return 0;
}

static int parse_route(char *s, struct configuration *c)
{
  struct config_route *r, *r_p;
  char *comma;

  if ((r = calloc(1, sizeof(*r))) == NULL) 
  {
    LOG_ERROR("alloc failed");
    return -1;
  }

  /* split destination from next hop */
  if ((comma = strchr(s, ',')) == NULL) 
  {
    LOG_ERROR("no comma found (%s)", s);
    goto failed;
  }
  *comma = 0;

  /* parse destination */
  r->ip_prefix = 32;
  if (parse_cidr(s, &r->ip, &r->ip_prefix) != 0) 
  {
    LOG_ERROR("pasing destination (%s) failed");
    goto failed;
  }

  /* parse next hop */
  if (parse_ipv4(comma + 1, &r->next_hop_ip) != 0) 
  {
    LOG_ERROR("parsing next hop (%s) failed", comma + 1);
    goto failed;
  }

  /* add to route list */
  r->next = NULL;
  if (c->routes == NULL) 
  {
    c->routes = r;
  } else 
  {
    for (r_p = c->routes; r_p->next != NULL; r_p = r_p->next);
    r_p->next = r;
  }
  return 0;

failed:
  return -1;
}

int parse_ipv4(const char *s, uint32_t *ip)
{
  if (inet_pton(AF_INET, s, ip) != 1) 
  {
    return -1;
  }
  
  *ip = htonl(*ip);
  return 0;
}
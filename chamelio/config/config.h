#ifndef CONFIG_H_
#define CONFIG_H_

#include <stdint.h>

struct configuration {
  /* IP address for this host */
  uint32_t ip;
  /* IP prefix length for this host */
  uint8_t ip_prefix;
  /* List of routes */
  struct config_route *routes;

  /* Fast-path configurations */
  /* Enable checksum offload */
  uint32_t fp_xsumoffloads;
  /* Max number of fast-path cores */
  uint32_t fp_cores_max;

  /* DPDK configurations */
  /* DPDK extra argument count */
  int dpdk_argc;
  /* DPDK extra argument vector */
  char **dpdk_argv;
};

/** Route entry in configuration */
struct config_route {
  /** Destination IP address */
  uint32_t ip;
  /** Destination prefix length */
  uint8_t ip_prefix;
  /** Next hop IP */
  uint32_t next_hop_ip;
  /** Next pointer for route list */
  struct config_route *next;
};

int config_parse(struct configuration *c, int argc, char **argv);

#endif
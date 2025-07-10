#ifndef CONFIG_H_
#define CONFIG_H_

#include <stdint.h>

struct configuration {
  /* Fast-path configurations */
  uint32_t fp_xsumoffloads;
  uint32_t fp_cores_max;

  /* DPDK configurations */
  int dpdk_argc;
  char **dpdk_argv;
};

int config_parse(struct configuration *c, int argc, char **argv);

#endif
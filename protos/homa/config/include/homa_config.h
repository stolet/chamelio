#ifndef HOMA_CONFIG_H_
#define HOMA_CONFIG_H_

#include <linux/types.h>

/* Slow-path configuration */
struct homa_configuration {
  /* Protocol is running in a VM */
  __u32 virt;
};

/* Parse options: 0 on success, -1 on error */
int homa_config_parse(struct homa_configuration *c, int argc, char **argv);

#endif

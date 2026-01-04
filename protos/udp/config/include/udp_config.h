#ifndef UDP_CONFIG_H_
#define UDP_CONFIG_H_

#include <linux/types.h>

struct udp_configuration {
  /* Protocol is running in a VM */
  __u32 virt;
  /* Use GRE for network virtualization */
  __u32 virt_gre;
};

int udp_config_parse(struct udp_configuration *c, int argc, char **argv);

#endif

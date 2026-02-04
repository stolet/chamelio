#ifndef TCP_CONFIG_H_
#define TCP_CONFIG_H_

#include <linux/types.h>

struct tcp_configuration {
  /* Protocol is running in a VM */
  __u32 virt;
  /* Use GRE for network virtualization */
  __u32 virt_gre;
};

int tcp_config_parse(struct tcp_configuration *c, int argc, char **argv);

#endif

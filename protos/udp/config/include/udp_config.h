#ifndef UDP_CONFIG_H_
#define UDP_CONFIG_H_

#include <linux/types.h>

#define UDP_EBPF_PATH_LEN 64

struct udp_configuration {
  /* Protocol is running in a VM */
  __u32 virt;
  /* Use GRE for network virtualization */
  __u32 virt_gre;
  /* Size of the receive buffer in bytes */
  __u32 rxbuf_sz;
  /* Size of the transmit buffer in bytes */
  __u32 txbuf_sz;
  /* Number of elements in the app queues */
  __u32 appq_len;
  /* Number of elements in the bump queues */
  __u32 bumpq_len;
  /* Number of elements in the control queues */
  __u32 ctlq_len;
  /* eBPF bytecode path selected for the fast-path */
  char ebpf_path[UDP_EBPF_PATH_LEN];
};

int udp_config_parse(struct udp_configuration *c, int argc, char **argv);

#endif

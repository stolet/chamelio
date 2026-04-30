#ifndef UDP_STATE_H_
#define UDP_STATE_H_

#include <linux/types.h>

#define UDP_STATE_PATH "/run/chamelio/udp_statetool"
#define UDP_STATE_MAGIC 0x7564707374617465ULL
#define UDP_STATE_VERSION 1

struct udp_state {
  __u64 magic;
  __u32 version;
  __u32 pid;
  __u64 ctx_addr;
  __u32 ctx_size;
  __u32 sock_size;
  __u32 port_size;
  __u32 cfg_size;
  __u32 app_size;
  __u32 app_ctx_size;
};

#endif

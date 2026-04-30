#ifndef TCP_STATE_H_
#define TCP_STATE_H_

#include <linux/types.h>

#define TCP_STATE_PATH "/run/chamelio/tcp_statetool"
#define TCP_STATE_MAGIC 0x7470637374617465ULL
#define TCP_STATE_VERSION 1

struct tcp_state {
  __u64 magic;
  __u32 version;
  __u32 pid;
  __u64 ctx_addr;
  __u32 ctx_size;
  __u32 sock_size;
  __u32 port_size;
  __u32 flow_bucket_size;
  __u32 ctl_cfg_size;
  __u32 listener_size;
  __u32 meta_size;
};

#endif

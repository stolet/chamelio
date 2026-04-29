#ifndef TCP_TRACE_H_
#define TCP_TRACE_H_

#include <linux/types.h>

#define TCP_TRACE_PATH "/run/chamelio/tcp_tracetool"
#define TCP_TRACE_MAGIC 0x7470637472616365ULL
#define TCP_TRACE_VERSION 1

struct tcp_trace_state {
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

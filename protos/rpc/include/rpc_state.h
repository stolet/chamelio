#ifndef RPC_STATE_H_
#define RPC_STATE_H_

#include <linux/types.h>

#define RPC_STATE_PATH "/run/chamelio/rpc_statetool"
#define RPC_STATE_MAGIC 0x7564707374617465ULL
#define RPC_STATE_VERSION 1

struct rpc_state {
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

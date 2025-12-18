#ifndef UDP_H_
#define UDP_H_

#include <linux/types.h>

/* Maximum segment size for UDP */
#define UDP_MSS 1400
/* TODO: Pass MAX_APPS and MAX_CTXS as a parameter to UDP slow-path */
/* Max number of applications that can register with slow-path */
#define MAX_APPS 8
/* Max number of contexts per application */
#define MAX_CTXS 8
/* Minumum port number */
#define MIN_PORT 1002
/* Maximum port number */
#define MAX_PORT 65534
/* Maximum number of UDP sockets */
#define MAX_SOCKETS MAX_PORT
/* Location where ebpf bytecode is located */
#define RPC_EBPF_BYTECODE "protos/rpc/fast/rpc_fast.bpf.o"

#endif
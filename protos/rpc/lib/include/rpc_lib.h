#ifndef UDP_LIB_H_
#define UDP_LIB_H_

#include <linux/types.h>
#include <sys/socket.h>

#include "queue.h"

/* TODO: Fix this because it's duplicated */
#define MAX_SOCKETS 8192
#define SOCK_INACTIVE (-1U)

struct rpc_socket_lib {
    /* File descriptor identifier for this socket */
    int fd;
    /* Socket ID in the slow-path */
    __u32 sock_id;
    /* Context that created this socket */
    struct rpc_context_lib *ctx;
    /* Fast-path core of this socket */
    __u16 core;

    /* RX port */
    __u16 rx_port;
    /* RX IP address */
    __u32 rx_ip;
    /* TX port */
    __u16 tx_port;
    /* TX IP address */
    __u32 tx_ip;

    /* Queue ID used for RX buffer */
    __u16 rx_qid;
    /* Length of RX buffer */
    __u32 rx_len;
    /* Number of available bytes to be read */
    __u32 rx_avail;
    /* Head of RX buffer */
    __u32 rx_head;
    /* Pointer to start of RX buffer in shared memory */
    void *rx_buf;
    
    /* Queue ID used for TX buffer */
    __u16 tx_qid;
    /* Length of the TX buffer */
    __u32 tx_len;
    /* Number of bytes written to buffer */
    __u32 tx_avail;
    /* Head of the TX buffer */
    __u32 tx_head;
    /* Pointer to the start of the TX buffer in shared memory */
    void *tx_buf;

    /* Result from bind. Default is -1 and is set 
       to 1 on success and 0 on failure */
    int bind_success;
    /* Result from setsockopt. Default is -1 and is set
       to 1 on success and 0 on failure */
    int setopt_success;
};

struct rpc_context_lib {
  /* ID for this context */
  __u16 id;

  /* Queue from app context to slow-path */
  struct equeue *app_slow_q;  
  /* Queue from slow-path to app context */
  struct dqueue *slow_app_q;
  
  /* Outgoing and incoming queue for each fast-path core*/
  __u16 ncores;
  struct equeue **app_fast_qs;
  struct dqueue **fast_app_qs;
};

struct rpc_lib {
  /* Unix socket file descriptor */
  int uxsocket_fd;

  /* Shared memory file descriptor */
  int shm_fd;
  /* Base pointer to mapped shared memory */
  void *shm_base;
  
  /* Next ctx ID */
  int next_ctxid;

  /* Next socket fd */
  int next_sockfd;
  /* Table with sockets */
  struct rpc_socket_lib socks[MAX_SOCKETS];
};

/* Connects to the slow-path */
int rpc_connect_slow();
/* Creates a new context for a thread */
struct rpc_context_lib * rpc_ctx_new();
/* Polls message queue for slow-path messages */
int rpc_poll_slow(struct rpc_context_lib *ctx);
/* Polls message queue for fast-path messages*/
int rpc_poll_fast(struct rpc_context_lib *ctx);

#endif
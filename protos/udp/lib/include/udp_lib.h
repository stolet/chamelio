#ifndef UDP_LIB_H_
#define UDP_LIB_H_

#include <linux/types.h>
#include <sys/socket.h>

#include "queue.h"

#define MAX_SOCKETS 8192
#define SOCK_INACTIVE (-1U)

struct udp_socket {
    /* File descriptor identifier for this socket */
    int fd;
    /* Socket ID in the slow-path */
    __u32 sock_id;
    /* Context that created this socket */
    struct udp_context_lib *ctx;
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
};

struct udp_context_lib {
  /* ID for this context */
  __u16 id;
  /* ID for this context in slow-path */
  __u16 id_slow;

  /* Queue from app context to slow-path */
  struct equeue *app_slow_q;  
  /* Queue from slow-path to app context */
  struct dqueue *slow_app_q;
  
  /* Outgoing and incoming queue for each fast-path core*/
  __u16 ncores;
  struct equeue **app_fast_qs;
  struct dqueue **fast_app_qs;
};

struct udp_lib {
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
  struct udp_socket socks[MAX_SOCKETS];
};

/* Connects to the slow-path */
int udp_connect_slow();
/* Creates a new context for a thread */
int udp_ctx_new();
/* Polls message queue for slow-path messages */
int udp_poll_slow();
/* Polls message queue for fast-path messages*/
int udp_poll_fast();
/* Creates a new UDP socket */
int udp_socket();
/* Binds src address to UDP socket */
int udp_bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
/* Sends data in buffer to the specified address */
int udp_sendto(int sockfd, const void *buf, size_t len, 
    const struct sockaddr *addr, socklen_t addr_len);
/* Reads data from socket buffer */
int udp_recvfrom(int sockfd, void *buf, size_t len, 
    struct sockaddr *addr, socklen_t addr_len);

#endif
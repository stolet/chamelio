#ifndef UDP_LIB_H_
#define UDP_LIB_H_

#include <stdint.h>
#include <sys/socket.h>

#include "queue.h"

#define MAX_SOCKETS 8192
#define SOCK_INACTIVE (-1U)

struct udp_socket {
    /* File descriptor identifier for this socket */
    int fd;
    /* Context that created this socket */
    struct udp_context_lib *ctx;
    /* Fast-path core of this socket */
    uint16_t core;

    /* Queue ID used for RX buffer */
    uint16_t rx_qid;
    /* Length of RX buffer */
    uint32_t rx_len;
    /* Number of available bytes to be read */
    uint32_t rx_avail;
    /* Head of RX buffer */
    uint32_t rx_head;
    /* Pointer to start of RX buffer in shared memory */
    void *rx_buf;
    
    /* Queue ID used for TX buffer */
    uint16_t tx_qid;
    /* Length of the TX buffer */
    uint32_t tx_len;
    /* Number of bytes written to buffer */
    uint32_t tx_avail;
    /* Head of the TX buffer */
    uint32_t tx_head;
    /* Pointer to the start of the TX buffer in shared memory */
    void *tx_buf;
};

struct udp_context_lib {
  /* ID for this context */
  uint16_t id;
  /* ID for this context in slow-path */
  uint16_t id_slow;

  /* Queue from app context to slow-path */
  struct equeue *app_slow_q;  
  /* Queue from slow-path to app context */
  struct dqueue *slow_app_q;
  
  /* Outgoing and incoming queue for each fast-path core*/
  uint16_t ncores;
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

int udp_connect_slow();
int udp_ctx_new();
int udp_poll_slow();
int udp_socket();
int udp_close(int sockfd);
int udp_sendto(int sockfd, const void *buf, size_t len, 
    const struct sockaddr *addr, socklen_t addr_len);
int udp_recvfrom(int sockfd, void *buf, size_t len, 
    struct sockaddr *addr, socklen_t addr_len);

#endif
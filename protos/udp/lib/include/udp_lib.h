#ifndef UDP_LIB_H_
#define UDP_LIB_H_

#include <stdint.h>
#include <sys/socket.h>

#include "queue.h"

struct udp_context_lib {
  /* ID for this context */
  uint16_t id;

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
};

struct udp_socket {
    /* File descriptor identifier for this socket */
    int fd;
    /* Length of RX buffer */
    uint32_t rx_len;
    /* Number of available bytes to be read */
    uint32_t rx_avail;
    /* Head of RX buffer */
    uint32_t rx_head;
    /* Pointer to start of RX buffer in shared memory */
    void *rx_buf;
    
    /* Length of the TX buffer */
    uint32_t tx_len;
    /* Number of bytes written to buffer */
    uint32_t tx_avail;
    /* Head of the TX buffer */
    uint32_t tx_head;
    /* Pointer to the start of the TX buffer in shared memory */
    void *tx_buf;
};

int udp_connect_slow();
int udp_ctx_new();
int udp_socket();
int udp_bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
int udp_close(int sockfd);
int udp_listen(int sockfs, int backlog);
int udp_write(int sockfd, const void *buf, size_t count);
int udp_read(int sockfd, void *buf, size_t count);

#endif
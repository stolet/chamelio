#ifndef TCP_LIB_H_
#define TCP_LIB_H_

#include <linux/types.h>
#include <sys/socket.h>

#include "queue.h"

/* TODO: Fix this because it's duplicated */
#define MAX_SOCKETS 8192
#define SOCK_INACTIVE (-1U)

#define TCP_WAIT_NONBLOCK (1U << 0)

#define TCP_WAIT_IN (1U << 0)
#define TCP_WAIT_OUT (1U << 1)
#define TCP_WAIT_ACCEPT (1U << 2)
#define TCP_WAIT_CONNECTED (1U << 3)
#define TCP_WAIT_SOCKET (1U << 4)
#define TCP_WAIT_BIND (1U << 5)
#define TCP_WAIT_SETOPT (1U << 6)
#define TCP_WAIT_OP (1U << 7)
#define TCP_WAIT_CLOSED (1U << 8)

struct tcp_wait_event {
    int sockfd;
    __u32 events;
    __u64 data;
};

struct tcp_socket_lib;

struct tcp_wait_entry {
    __u32 events;
    __u64 data;
    int index;
    struct tcp_socket_lib *sock;
};

struct tcp_wait {
    struct tcp_context_lib *ctx;
    int nfds;
    int sockfds[MAX_SOCKETS];
    struct tcp_wait_entry entries[MAX_SOCKETS];
};

enum tcp_sock_state {
  TCP_LIB_STATE_INIT = 0,
  TCP_LIB_STATE_BOUND,
  TCP_LIB_STATE_LISTEN,
  TCP_LIB_STATE_CONNECTING,
  TCP_LIB_STATE_ESTABLISHED,
  TCP_LIB_STATE_CLOSING,
  TCP_LIB_STATE_CLOSED,
};

#define TCP_LIB_STATUS_IDLE (-2)
#define TCP_LIB_STATUS_PENDING (-1)

struct tcp_socket_lib {
    /* File descriptor identifier for this socket */
    int fd;
    /* Socket ID in the slow-path */
    __u32 sock_id;
    /* Context that created this socket */
    struct tcp_context_lib *ctx;
    /* Fast-path core of this socket */
    __u16 core;

    /* Local port */
    __u16 local_port;
    /* Local IP address */
    __u32 local_ip;
    /* Remote port */
    __u16 remote_port;
    /* Remote IP address */
    __u32 remote_ip;

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
    /* Number of bytes still owned by TCP in the TX buffer */
    __u32 tx_avail;
    /* Head of the TX buffer, points to the oldest buffered byte */
    __u32 tx_head;
    /* Pointer to the start of the TX buffer in shared memory */
    void *tx_buf;

    /* Result from bind. Default is -1 and is set
       to 1 on success and 0 on failure */
    int bind_success;
    /* Result from setsockopt. Default is -1 and is set
       to 1 on success and 0 on failure */
    int setopt_success;
    /* Result from connect/listen/accept. 0 on success errno on failure */
    int op_status;
    /* Number of established children waiting on accept() */
    __u32 pending_conn;
    /* Socket state mirrored in the library */
    __u8 state;
    /* 1 if SO_REUSEPORT was enabled */
    __u8 reuseport;
};

struct tcp_context_lib {
  /* ID for this context */
  __u16 id;

  /* Queue from app context to slow-path */
  struct equeue *app_slow_q;
  /* Queue from slow-path to app context */
  struct dqueue *slow_app_q;

  /* Outgoing and incoming queue for each fast-path core */
  __u16 ncores;
  struct equeue **app_fast_qs;
  struct dqueue **fast_app_qs;
};

struct tcp_lib {
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
  struct tcp_socket_lib socks[MAX_SOCKETS];
};

/* Connects to the slow-path */
int tcp_connect_slow();
/* Creates a new context for a thread */
struct tcp_context_lib * tcp_ctx_new();
/* Creates a new wait object for a context */
struct tcp_wait *tcp_wait_new(struct tcp_context_lib *ctx);
/* Destroys a wait object */
void tcp_wait_free(struct tcp_wait *wait);
/* Adds a socket to a wait object */
int tcp_wait_add(struct tcp_wait *wait, int sockfd, __u32 events,
    __u64 data);
/* Modifies a registered socket in a wait object */
int tcp_wait_mod(struct tcp_wait *wait, int sockfd, __u32 events,
    __u64 data);
/* Removes a socket from a wait object */
int tcp_wait_del(struct tcp_wait *wait, int sockfd);
/* Waits for fast-path and slow-path events */
int tcp_wait(struct tcp_wait *wait, struct tcp_wait_event *events,
    int maxevents, int flags);
/* Waits for fast-path events only */
int tcp_wait_fast(struct tcp_wait *wait, struct tcp_wait_event *events,
    int maxevents, int flags);
/* Waits for slow-path events only */
int tcp_wait_slow(struct tcp_wait *wait, struct tcp_wait_event *events,
    int maxevents, int flags);
/* Polls message queue for slow-path messages */
int tcp_poll_slow(struct tcp_context_lib *ctx);
/* Polls message queue for fast-path messages */
int tcp_poll_fast(struct tcp_context_lib *ctx);
/* Creates a new TCP socket */
int tcp_socket(struct tcp_context_lib *ctx);
/* Binds src address to TCP socket */
int tcp_bind(struct tcp_context_lib *ctx, int sockfd,
    const struct sockaddr *addr, socklen_t addrlen);
/* Initiates a TCP connection */
int tcp_connect(struct tcp_context_lib *ctx, int sockfd,
    const struct sockaddr *addr, socklen_t addrlen, int flags);
/* Marks a socket as listening */
int tcp_listen(struct tcp_context_lib *ctx, int sockfd, int backlog);
/* Accepts a new TCP connection */
int tcp_accept(struct tcp_context_lib *ctx, int sockfd,
    struct sockaddr *addr, socklen_t *addrlen, int flags);
/* Sets socket options */
int tcp_setsockopt(struct tcp_context_lib *ctx, int sockfd, __u8 opt);
/* Shuts down a TCP socket */
int tcp_shutdown(struct tcp_context_lib *ctx, int sockfd, int how);
/* Sends data in buffer to the specified address */
int tcp_sendto(struct tcp_context_lib *ctx, int sockfd,
    const void *buf, size_t len,
    const struct sockaddr *addr, socklen_t addr_len);
/* Reads data from socket buffer */
int tcp_recvfrom(struct tcp_context_lib *ctx, int sockfd,
    void *buf, size_t len,
    struct sockaddr *addr, socklen_t addr_len);
/* Closes a TCP socket */
int tcp_close(struct tcp_context_lib *ctx, int sockfd);

#endif

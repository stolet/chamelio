#ifndef UDP_SLOW_H_
#define UDP_SLOW_H_

#include <cham_lib.h>

#define MAX_APPS 8
#define MAX_CTXS 8
#define MAX_SOCKETS 8192

struct udp_socket_slow {
  /* Socket ID */
  int id;

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

struct udp_app_context_slow {
  /* ID for application context */
  uint8_t id;
  /* Application this context belongs to */
  struct udp_app_slow *app;
  /* Queue for messages app->slow */
  struct dqueue *app_slow_q;
  /* Queue for messages slow->app */
  struct equeue *slow_app_q;
};

struct udp_app_slow {
  /* ID of the application */
  uint8_t id;
  /* Number of registered application contexts */
  uint8_t n_ctxs;
  /* List of application contexts */
  struct udp_app_context_slow ctxs[MAX_CTXS];
  /* Registered sockets */
  int n_socks;
  /* Map of sockets */
  struct proto_map_lib *socks;
};

struct udp_slow_context {
  /* Listening UX sockets for new applications */
  int app_uxfd;
  /* Epoll object used by UX application socket */
  int app_epfd;

  /* Chamelio library guest structure */
  struct guest_lib *guest;
  /* Chamelio library protocol structure */
  struct proto_lib *proto;

  /* Number of registered applications */
  uint8_t n_apps;
  /* Apps that have registered with chamelio */
  struct udp_app_slow apps[MAX_APPS];
};

#endif
#ifndef TCP_SLOW_H_
#define TCP_SLOW_H_

#include <cham_lib.h>
#include <cham_fast.h>

#include "tcp.h"
#include "tcp_config.h"
#include "tomgr.h"
#include "vfio.h"

/* Size of batch size used to poll queues */
#define SLOW_BATCH_SIZE 16

struct tcp_app_context_slow {
  /* ID for application context */
  __u8 id;
  /* Application this context belongs to */
  struct tcp_app_slow *app;
  /* Queue for messages app->slow */
  struct dqueue *app_slow_q;
  /* Queue for messages slow->app */
  struct equeue *slow_app_q;
  /* App bump queues */
  struct proto_queue_lib *app_bump_qs[MAX_FP_CORES];
  /* Fast-path bump queues */
  struct proto_queue_lib *fast_bump_qs[MAX_FP_CORES];
};

struct tcp_app_slow {
  /* ID of the application */
  __u8 id;
  /* Number of registered application contexts */
  __u8 n_ctxs;
  /* Next context to poll */
  __u8 next_ctx;
  /* List of application contexts */
  struct tcp_app_context_slow ctxs[MAX_CTXS];
};

struct tcp_listener_slow {
  /* 1 when this listener is active */
  __u8 active;
  /* Listening socket ID */
  __u32 sock_id;
  /* Maximum number of pending children */
  __u32 backlog_len;
  /* Number of half-open or ready children tracked here */
  __u32 backlog_used;
  /* Head of ready child ring */
  __u32 ready_head;
  /* Number of established children ready for accept */
  __u32 ready_used;
  /* Ring of socket IDs that are ready to be accepted */
  __u32 *ready_sids;
};

struct tcp_sock_meta_slow {
  /* Listener this socket belongs to before accept() attaches it */
  __u32 listener_id;
  /* 1 if connect() auto-selected the local port */
  __u8 auto_bound;
  /* Retransmission timer entry for this socket */
  struct to_entry *timer;
  /* Retransmission kind currently armed for this socket */
  __u8 retx_kind;
  /* Number of retransmissions left after the initial send */
  __u8 retx_left;
};

struct tcp_slow_context {
  /* TCP confguration */
  struct tcp_configuration config;
  /* Listening UX sockets for new applications */
  int app_uxfd;
  /* Epoll object used by UX application socket */
  int app_epfd;
  /* Number of sockets registered */
  __u32 n_socks;

  /* Chamelio library protocol structure */
  struct proto_lib *proto;

  /* Number of registered applications */
  __u8 n_apps;
  /* Next app to poll */
  __u8 next_app;
  /* Apps that have registered with chamelio */
  struct tcp_app_slow apps[MAX_APPS];

  /* Signal queue used by fast-path to punt TCP control packets */
  struct dqueue *fast_slow_sig_q;
  /* Packet queue used by fast-path to punt TCP control packets */
  struct dqueue *fast_slow_pkt_q;
  /* Signal queue used by slow-path to send TCP control packets */
  struct equeue *slow_fast_sig_q;
  /* Packet queue used by slow-path to send TCP control packets */
  struct equeue *slow_fast_pkt_q;
  /* Timeout manager used for TCP control-packet retransmissions */
  struct tomgr *tomgr;

  /* Chamelio socket map */
  struct proto_map_lib *socks_map;
  /* Maps listening ports to sockets for fast-path lookup */
  struct proto_map_lib *port_map;
  /* Maps 4-tuples to sockets for fast-path lookup */
  struct proto_map_lib *flow_map;
  /* Shared protocol config visible to the fast-path */
  struct proto_map_lib *ctrl_map;

  /* Slow-path listener state indexed by socket ID */
  struct tcp_listener_slow *listeners;
  /* Slow-path metadata indexed by socket ID */
  struct tcp_sock_meta_slow *sock_meta;
  /* Slow-path port reservations for bind/connect */
  struct tcp_port *bound_ports;
};

#endif

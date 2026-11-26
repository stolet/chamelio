#ifndef RPC_SLOW_H_
#define RPC_SLOW_H_

#include <cham_lib.h>
#include <cham_fast.h>

#include "rpc.h"
#include "rpc_slow.h"
#include "rpc_config.h"
#include "rpc_queue_types.h"

/* Size of batch size used to poll queues */
#define BATCH_SIZE 16

/* Size of RPC transmit buffer in bytes */
#define TXBUF_SZ 32768
/* Size of RPC receive buffer in bytes */
#define RXBUF_SZ 32768
/* Number of elements in bump queue */
#define BUMPQ_SZ 16384
/* Number of elements in queue between slow-path and app */
#define APPQ_SZ 128

struct rpc_app_context_slow
{
  /* ID for application context */
  __u8 id;
  /* Application this context belongs to */
  struct rpc_app_slow *app;
  /* Queue for messages app->slow */
  struct dqueue *app_slow_q;
  /* Queue for messages slow->app */
  struct equeue *slow_app_q;
  /* App bump queues */
  struct proto_queue_lib *app_bump_qs[MAX_FP_CORES];
  /* Fast-path bump queues */
  struct proto_queue_lib *fast_bump_qs[MAX_FP_CORES];
};

struct rpc_app_slow
{
  /* ID of the application */
  __u8 id;
  /* Number of registered application contexts */
  __u8 n_ctxs;
  /* Next context to poll */
  __u8 next_ctx;
  /* List of application contexts */
  struct rpc_app_context_slow ctxs[MAX_CTXS];
};

struct rpc_slow_context
{
  /* RPC configuration */
  struct rpc_configuration config;
  /* Listening UX sockets for new applications */
  int app_uxfd;
  /* Epoll object used by UX application socket */
  int app_epfd;
  /* Number of sockets registered */
  __u32 n_socks;

  /* Chamelio library guest structure */
  // struct guest_lib *guest;
  /* Chamelio library protocol structure */
  struct proto_lib *proto;

  /* Number of registered applications */
  __u8 n_apps;
  /* Next app to poll */
  __u8 next_app;
  /* Apps that have registered with chamelio */
  struct rpc_app_slow apps[MAX_APPS];

  /* Chamelio socket map */
  struct proto_map_lib *socks_map;
  /* Maps a port to a socket */
  struct proto_map_lib *port_map;
  /* Fast-path ephemeral-port allocation state */
  struct proto_map_lib *cfg_map;

  /* Stores all the clients */
  struct proto_map_lib *clients_map;
  /* Number of clients */
  __u32 n_clients;

  // /* Maps a port to a server */
  // struct proto_map_lib *port_server_map;
  /* Stores all the servers */
  struct proto_map_lib *servers_map;
  /* Stores all the workers */
  struct proto_map_lib *workers_map;
  /* Number of servers */
  __u32 n_servers;
  /* Number of workers */
  __u32 n_workers;
};

#endif

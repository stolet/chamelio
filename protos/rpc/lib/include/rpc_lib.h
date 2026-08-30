#ifndef RPC_LIB_H_
#define RPC_LIB_H_

#include <linux/types.h>
#include <sys/socket.h>

#include "queue.h"

/* TODO: Fix this because it's duplicated */
#define MAX_SERVERS 6
#define MAX_CLIENTS 256
#define MAX_WORKERS 16

#define MAX_SOCKETS 8192
#define INVALID_ID -1
#define SOCK_INACTIVE (-1U)

enum rpc_req_class
{
  RPC_REQ_CLASS_SHORT = 1,
  RPC_REQ_CLASS_LONG = 2,
};

enum worker_type
{
  WORKER_TYPE_SHORT = 1,
  WORKER_TYPE_LONG = 2,
};

struct rpc_context_lib
{
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

/* Worker that handles RPC requests */
struct rpc_worker_lib
{
  /* Context that created this worker */
  struct rpc_context_lib *ctx;
  /* Server for this worker */
  struct rpc_server_lib *server;

  /* Fast-path id of this worker */
  __u16 worker_id;

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
  /* Total size of the current in-flight request (hdr + payload) */
  __u32 rx_pkt_len;
  /* eBPF mode: BUMP_APP_RX has been peeked but not dequeued yet. */
  __u8 rx_pending;

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
  /* Pointer to the worker state in shared memory */
  void *shm_worker;

  /* Dispatcher write pointer for mode 2 (app-layer JSQ).
   * Only the dispatcher thread advances this; the worker only reads rx_head. */
  __u32 rx_disp_tail;
};

/* RPC client that makes calls */
struct rpc_client_lib
{
  // /* Type field (= Client) */
  // __u8 type;
  /* Context that created this client */
  struct rpc_context_lib *ctx;
  /* Fast-path core of this client */
  __u16 core;

  /* ID of this client */
  __u16 client_id;

  // /* Next request ID */
  // __u32 next_rid;

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
};

/* RPC server that registers different services */
struct rpc_server_lib
{
  /* Context that created this client */
  struct rpc_context_lib *ctx;

  /* ID of this server */
  __u32 id;

  /* Fast-path core of this server */
  __u16 core;

  /* Number of registered services */
  __u16 nservices;
  /* Array of registered services */
  __u16 *services;

  /* Number of workers */
  __u16 nworkers;
  /* Array of workers */
  struct rpc_worker_lib workers[MAX_WORKERS];

  /* Result from bind. Default is -1 and is set
      to 1 on success and 0 on failure */
  int bind_success;

  /* App-layer LB fields */
  __u8 app_lb_mode;
  /* 0 = JSQ (unbounded); > 0 = bounded queue (use JOB_QUEUE_SIZE to revert) */
  __u32 job_queue_bound;
  /* Pointer to server state in shared memory */
  void *shm_server;
  /* Pointer to the server-level shared RX ring in shared memory */
  void *rx_buf;
  /* Length of the shared RX ring */
  __u32 rx_len;
};

struct rpc_lib
{
  /* Unix socket file descriptor */
  int uxsocket_fd;

  /* Shared memory file descriptor */
  int shm_fd;
  /* Base pointer to mapped shared memory */
  void *shm_base;

  /* Next ctx ID */
  int next_ctxid;
  /* Next client ID */
  int next_clientid;
  /* Next server ID */
  int next_serverid;
  /* Next rid */
  __u32 next_rid;

  /* Table with servers */
  struct rpc_server_lib servers[MAX_SERVERS];
  /* Table with clients */
  struct rpc_client_lib clients[MAX_CLIENTS];
};

/* Connects to the slow-path */
int rpc_connect_slow();
/* Creates a new context for a thread */
struct rpc_context_lib *rpc_ctx_new();
/* Polls message queue for slow-path messages */
int rpc_poll_slow(struct rpc_context_lib *ctx);
/* Polls message queue for RPC requests */
int rpc_poll_calls(struct rpc_context_lib *ctx);

/* Allocates an RPC client */
struct rpc_client_lib *rpc_new_client(struct rpc_context_lib *ctx, __u32 ip, __u16 port);
/* Allocates an RPC server */
struct rpc_server_lib *rpc_new_server(struct rpc_context_lib *ctx, __u32 ip, __u16 port);
/* Creates a new worker for a server */
struct rpc_worker_lib *rpc_new_worker(struct rpc_server_lib *s, struct rpc_context_lib *ctx);
/* Registers a service with the given server */
int rpc_register(struct rpc_server_lib *server, __u8 service);
/* Sends an RPC request to the given IP and port */
int rpc_call(struct rpc_client_lib *c, __u32 ip, __u16 port,
             __u16 service, void *buf, size_t len);
/* Sends an RPC response for a call that was handled */
int rpc_return(struct rpc_server_lib *s, struct rpc_worker_lib *w,
               __u32 rid, void *buf, size_t len);
/* Parses the top request in worker queue and returns the RID for the request */
int rpc_handle_call(struct rpc_worker_lib *w, __u32 *rid,
                    void *buf, size_t len);
/* Parses the response from a worker */
int rpc_response(struct rpc_client_lib *c, void *buf, size_t len);

/* Removes a pending job from the given worker.
Information used by FP during LB of workers */
int rpc_call_complete(struct rpc_worker_lib *w);

/* Switch server to app-layer pull mode (JBSQ when job_queue_bound > 0, else unbounded) */
int rpc_set_app_lb(struct rpc_server_lib *server);

/* Switch server to app-layer JSQ dispatcher mode */
int rpc_set_app_jsq(struct rpc_server_lib *server);

/* Switch server to eBPF round-robin mode (fast-path LB, no app-layer dispatcher) */
int rpc_set_ebpf_rr(struct rpc_server_lib *server);

/* Switch server to eBPF LWL mode; cost_table[service_id] = cost units per request */
int rpc_set_ebpf_lwl(struct rpc_server_lib *server,
                     const __u32 *cost_table, __u16 n_costs);

/* Complete a request and subtract its service cost from work_remaining */
int rpc_call_complete_svc(struct rpc_worker_lib *w, __u8 service_id);

/* Dispatch one pending request from the shared ring to the worker with fewest
 * jobs_pending. Call in a tight loop from the dispatcher thread. */
int rpc_app_dispatch(struct rpc_server_lib *server);
/* Set the service class for a given service ID */
int rpc_set_service_class(struct rpc_server_lib *server,
                          __u8 service_id, __u8 class);
/* Classify a worker as serving short or long requests. */
int rpc_set_worker_type(struct rpc_worker_lib *worker, __u8 worker_type);

#endif

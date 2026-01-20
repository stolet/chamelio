#ifndef RPC_QUEUE_H_
#define RPC_QUEUE_H_

#include <linux/types.h>
#include <cham_fast.h>

#include "utils.h"

/* Type of queue entries */
enum rpc_queue_type
{
  /* Signals that the queue is empty */
  RPC_QUEUE_EMPTY = 0,
  /* Request to slow-path to register new app */
  RPC_QUEUE_NEW_ACTX_REQ,
  /* Response from slow-path after registering app */
  RPC_QUEUE_NEW_ACTX_RES,
  /* Request to slow-path to create new socket */
  RPC_QUEUE_NEW_SOCK_REQ,
  /* Response from slow-path for created socket */
  RPC_QUEUE_NEW_SOCK_RES,
  /* Sets a new option for socket */
  RPC_QUEUE_SETOPT_REQ,
  /* Sets a new response for socket */
  RPC_QUEUE_SETOPT_RES,
  /* Bind message for a socket */
  RPC_QUEUE_BIND_REQ,
  /* Returns if bind was successful */
  RPC_QUEUE_BIND_RES,
  /* Bumps Chamelio when app calls send */
  RPC_QUEUE_BUMP_CHAM_TX,
  /* Bump Chamelio when app calls recv */
  RPC_QUEUE_BUMP_CHAM_RX,
  /* Bump app when Chamelio sends a packet */
  RPC_QUEUE_BUMP_APP_TX,
  /* Bump app when Chamelio receives a packet */
  RPC_QUEUE_BUMP_APP_RX,
  /* Request to create new client in slow-path */
  RPC_QUEUE_NEW_CLIENT_REQ,
  /* Response for new client created */
  RPC_QUEUE_NEW_CLIENT_RES,
  /* Request to create new server in slow-path */
  RPC_QUEUE_NEW_SERVER_REQ,
  /* Response for new server created */
  RPC_QUEUE_NEW_SERVER_RES,
  /* Request to create new worker in slow-path */
  RPC_QUEUE_NEW_WORKER_REQ,
  /* Response for new worker created */
  RPC_QUEUE_NEW_WORKER_RES,
};

struct rpc_queue_new_worker_req
{
  /* Opaque of the server this worker is for, way easier than extracting the ID*/
  __u32 server_id;
  /* Opaque pointer to worker in app library */
  __u64 opaque;

} __attribute__((packed));

struct rpc_queue_new_worker_res
{
  /* ID of the worker in slow-path */
  __u32 worker_id;
  /* Opaque pointer to worker in app library */
  __u64 opaque;
  /* Queue ID of RX buffer */
  __u16 rx_qid;
  /* Length of RX buffer */
  __u32 rx_len;
  /* Offset of RX buffer */
  __u64 rx_off;
  /* Queue ID of TX buffer */
  __u16 tx_qid;
  /* Length of TX buffer */
  __u32 tx_len;
  /* Offset of TX buffer */
  __u64 tx_off;
} __attribute__((packed));

struct rpc_queue_new_server_req
{ 
  /* IP bound to the server */
  __u32 local_ip;
  /* Port bound to the server */
  __u16 local_port;
  /* Opaque pointer to server in app library */
  __u64 opaque;
} __attribute__((packed));

struct rpc_queue_new_server_res
{
  /* ID of the server in slow-path */
  __u32 server_id;
  /* Opaque pointer to server in app library */
  __u64 opaque;
  /* Fast-path core this server is running */
  __u16 core;
  /* Signals if bind was successful */
  __u8 success;
} __attribute__((packed));

/* Request to create new client in slow-path */
struct rpc_queue_new_client_req
{ 
  /* IP of the client */
  __u32 local_ip;
  /* Port of the client */
  __u16 local_port;
  /* Opaque pointer to client in app library */
  __u64 opaque;
} __attribute__((packed));

/* Response for new client created */
struct rpc_queue_new_client_res
{ 
  //TBC: would we need client_id here?
  /* ID of the client in slow-path */
  __u32 client_id;
  /* Opaque pointer to client in app library */
  __u64 opaque;
  /* Fast-path core this client is running */
  __u16 core;
  /* Queue ID of RX buffer */
  __u16 rx_qid;
  /* Length of RX buffer */
  __u32 rx_len;
  /* Offset of RX buffer */
  __u64 rx_off;
  /* Queue ID of TX buffer */
  __u16 tx_qid;
  /* Length of TX buffer */
  __u32 tx_len;
  /* Offset of TX buffer */
  __u64 tx_off;
  /* Signals if bind was successful */
  __u8 success;
} __attribute__((packed));

/* Request to create new socket in slow-path */
struct rpc_queue_new_sock_req
{
  /* Pointer to socket in the library */
  __u64 opaque;
} __attribute__((packed));

/* Response for new socket created */
struct rpc_queue_new_sock_res
{
  /* ID of the socket in slow-path */
  __u32 sock_id;
  /* Pointer to socket in the library */
  __u64 opaque;
  /* Fast-path core this socket is running */
  __u16 core;
  /* Queue ID of RX buffer */
  __u16 rx_qid;
  /* Length of RX buffer */
  __u32 rx_len;
  /* Offset of RX buffer */
  __u64 rx_off;
  /* Queue ID of TX buffer */
  __u16 tx_qid;
  /* Length of TX buffer */
  __u32 tx_len;
  /* Offset of TX buffer */
  __u64 tx_off;
} __attribute__((packed));

/* Request for app context to register with slow-path */
struct rpc_queue_new_actx_req
{
  /* Add a byte value to request so it's not empty */
  __u8 req;
} __attribute__((packed));

/* Response when app context registers with slow-path */
struct rpc_queue_new_actx_res
{
  /* Number of fast-path cores */
  __u32 n_fp_cores;
  /* Size of shm region */
  __u32 shm_len;
  /* NUmber of elements app->slow queue */
  __u32 as_nelems;
  /* Size of elements app->slow queue */
  __u32 as_elsize;
  /* Offset in shm of app->slow queue */
  __u64 as_off;
  /* Number of elements slow->app queue */
  __u32 sa_nelems;
  /* Size of elements slow->app queue */
  __u32 sa_elsize;
  /* Offset in shm of slow->app queue */
  __u64 sa_off;
  /* Number of elements app->fast queues */
  __u32 af_nelems;
  /* Size of elements app->fast queues */;
  __u32 af_elsize;
  /* Offsets for app->fast bump queues */
  __u64 af_offs[MAX_FP_CORES];
  /* Number of elements fast->app queues */
  __u32 fa_nelems;
  /* Size of elements fast->app queues */
  __u32 fa_elsize;
  /* Offsets for fast->app bump queues */
  __u64 fa_offs[MAX_FP_CORES];
} __attribute__((packed));

/* Message that sends the src local_por and local_ip */
struct rpc_queue_bind_req
{
  /* Socket ID used by slow-path */
  __u32 sock_id;
  /* Source local_port for this bind */
  __u16 local_port;
  /* Source IP for this bind */
  __u32 local_ip;
  /* Opaque pointer to struct in app library */
  __u64 opaque;
} __attribute__((packed));

/* Message that signals if bind was successful */
struct rpc_queue_bind_res
{
  /* 0 for fail 1 for success */
  __u8 success;
  /* Opaque pointer to struct in app library */
  __u64 opaque;
} __attribute__((packed));

/* Message that sets an option for the socket */
struct rpc_queue_setopt_req
{
  /* Type of option to set */
  __u8 opt;
  /* Socket ID to set the option */
  __u32 sock_id;
  /* Opaque pointer to socket in app library */
  __u64 opaque;
} __attribute__((packed));

/* Response that signals if setting an option was successful */
struct rpc_queue_setopt_res
{
  /* 1 if request was successful 0 otherwise */
  __u8 success;
  /* Opaque pointer to socket in app library */
  __u64 opaque;
} __attribute__((packed));

/* Message that bumps the Chamelio TX avail */
struct rpc_queue_bump_cham_tx
{
  /* Socket ID used by slow-path */
  __u32 sock_id;
  /* TX local_port for this bump */
  __u16 tx_port;
  /* TX IP for this bump */
  __u32 tx_ip;
  /* Bump for TX available */
  __u32 tx_avail;
} __attribute__((packed));

/* Message that bumps the Chamelio RX head */
struct rpc_queue_bump_cham_rx
{
  /* Socket ID used by slow-path */
  __u32 sock_id;
  /* Bump for RX head */
  __u32 rx_head;
} __attribute__((packed));

/* Message that bumps the app TX head */
struct rpc_queue_bump_app_tx
{
  /* Opaque pointer to struct in app library */
  __u64 opaque;
  /* Bump for TX head */
  __u32 tx_head;
} __attribute__((packed));

/* Message that bumps the app RX available */
struct rpc_queue_bump_app_rx
{
  /* Opaque pointer to struct in app library */
  __u64 opaque;
  /* RX local_port for this bump */
  __u16 rx_port;
  /* RX IP for this bump */
  __u32 rx_ip;
  /* Bump for RX available */
  __u32 rx_avail;
} __attribute__((packed));

struct rpc_queue_entry
{
  /* Type of queue entry. Don't update outside of enqueue or dequeue */
  volatile __u8 type;
  /* Data section of queue entry */
  union
  {
    struct rpc_queue_new_actx_req new_actx_req;
    struct rpc_queue_new_actx_res new_actx_res;
    struct rpc_queue_new_sock_req new_sock_req;
    struct rpc_queue_new_sock_res new_sock_res;
    struct rpc_queue_bind_req bind_req;
    struct rpc_queue_bind_res bind_res;
    struct rpc_queue_setopt_req setopt_req;
    struct rpc_queue_setopt_res setopt_res;
    struct rpc_queue_new_client_req new_client_req;
    struct rpc_queue_new_client_res new_client_res;
    struct rpc_queue_new_server_req new_server_req;
    struct rpc_queue_new_server_res new_server_res;
    struct rpc_queue_new_worker_req new_worker_req;
    struct rpc_queue_new_worker_res new_worker_res;
    __u8 raw[511];
  } __attribute__((packed)) data;
} __attribute__((packed));

struct rpc_queue_bump_entry
{
  /* Type of queue entry. Don't upadete outside of enqueue or dequeue */
  volatile __u8 type;
  /* Data section of queue entry */
  union
  {
    struct rpc_queue_bump_app_tx bump_app_tx;
    struct rpc_queue_bump_app_rx bump_app_rx;
    struct rpc_queue_bump_cham_tx bump_cham_tx;
    struct rpc_queue_bump_cham_rx bump_cham_rx;
    /* Keeps queue entry the size of half a cache line */
    __u8 raw[31];
  } __attribute__((packed)) data;
} __attribute__((packed));

/* We want queue entries to be cache line sized for faster retrieval */
STATIC_ASSERT(sizeof(struct rpc_queue_entry) == 512, rpc_queue_entry_size);
STATIC_ASSERT(sizeof(struct rpc_queue_bump_entry) == 32, rpc_bump_queue_entry_size);

#endif

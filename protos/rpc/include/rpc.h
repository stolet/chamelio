#ifndef UDP_H_
#define UDP_H_

#include <linux/types.h>

/* Maximum segment size for UDP */
#define UDP_MSS 1400
/* TODO: Pass MAX_APPS and MAX_CTXS as a parameter to UDP slow-path */
/* Max number of applications that can register with slow-path */
#define MAX_APPS 8
/* Max number of contexts per application */
#define MAX_CTXS 8
/* Minumum port number */
#define MIN_PORT 1002
/* Maximum port number */
#define MAX_PORT 65534
/* Maximum number of UDP sockets */
// #define MAX_SOCKETS MAX_PORT
/* Maximum number of servers */
#define MAX_SERVERS 6
/* Maximum number of clients */
#define MAX_CLIENTS 256
/* Maximum number of workers */
#define MAX_WORKERS 16
/* Invalid server/worker ID */
#define INVALID_ID -1
/* Maximum number of services */
#define MAX_SERVICE_NUMBER 8
/* Location where ebpf bytecode is located */
#define RPC_EBPF_BYTECODE "protos/rpc/fast/rpc_fast.bpf.o"

// #define JOB_QUEUE_SIZE 1

/* Entry for the client map */
struct rpc_client
{
  /* Client ID */
  __u32 id;
  /* Fast-path core this client is currently running on */
  __u16 core;
  /* Queue ID to bump app */
  __u16 app_bump_qid;
  /* Opaque pointer to client in application */
  __u64 opaque;

  /* Local IP of the client */
  __u32 local_ip;
  /* Local port of the client */
  __u16 local_port;

  /* Length of RX buffer */
  __u32 rx_len;
  /* Number of available bytes to be read */
  __u32 rx_avail;
  /* Head of RX buffer */
  __u32 rx_head;
  /* Pointer to start of RX buffer in shared memory */
  __u64 rx_off;

  /* Length of the TX buffer */
  __u32 tx_len;
  /* Number of bytes written to buffer */
  __u32 tx_avail;
  /* Head of the TX buffer */
  __u32 tx_head;
  /* Offset to the start of the TX buffer in shared memory */
  __u64 tx_off;
} __attribute__((packed));

/* Entry for the port to server map */
struct rpc_port_entry
{
  /* Server ID */
  __u32 server_id;
  /* Client ID */
  __u32 client_id;
};

/* Metadata prepended to each entry in server's shared RX ring (app-lb mode).
   eBPF writes this before the RPC message so the dispatcher knows the source. */
struct rpc_rx_meta
{
  __u32 rx_ip;   /* source IP, host byte order */
  __u16 rx_port; /* source port, host byte order */
  __u16 _pad;
} __attribute__((packed));

struct rpc_server
{
  /* Server ID */
  __u32 id;
  /* First worker */
  __u32 workers[MAX_WORKERS];
  /* Number of workers */
  __u16 n_workers;
  /* Local IP */
  __u32 local_ip;
  /* Local port */
  __u16 local_port;
  /* Registered services for the server */
  __u8 service_table[MAX_SERVICE_NUMBER];

  /* App layer LB mode flag: 0: eBPF (default) */
  __u8 app_lb_mode;
  /* eBPF LB policy when app_lb_mode == 0: 0 = JSQ (default), 1 = round-robin */
  __u8 ebpf_lb_mode;
  /* Round-robin next worker index; written exclusively by the eBPF fast path */
  __u32 rr_next;
  /* Max in-flight jobs per worker when app_lb_mode == 1.
   * 0 = JSQ; > 0 = JBSQ with that bound */
  __u32 job_queue_bound;
  /* Shared RX ring used when app_lb_mode == 1 */
  /* Length of RX buffer */
  __u32 rx_len;
  /* Pointer to start of RX buffer in shared memory */
  __u64 rx_off;
  /* head advanced by app workers after consuming an entry */
  __u32 rx_head;
  /* start of unused bytes in RX buffer; advanced by fast path after publishing an entry */
  __u32 rx_tail;
  /* spinlock protecting app-LB consumer rx_head for multi-worker pull (0=free, 1=held) */
  __u32 rx_lock;
};

struct rpc_worker
{
  /* Worker ID */
  __u32 id;
  /* Opaque pointer to worker in application */
  __u64 opaque;
  /* Queue ID to bump app */
  __u16 app_bump_qid;
  /* Server ID */
  __u32 server_id;
  /* Number of pending jobs */
  __u32 jobs_pending;

  /* Length of RX buffer */
  __u32 rx_len;
  /* Pointer to start of RX buffer in shared memory */
  __u64 rx_off;
  /* Number of available bytes to be read */
  __u32 rx_avail;
  /* Head of RX buffer */
  __u32 rx_head;


  /* Length of TX buffer */
  __u32 tx_len;
  /* Pointer to start of TX buffer in shared memory */
  __u64 tx_off;
  /* Number of bytes written to buffer */
  __u32 tx_avail;
  /* Head of the TX buffer */
  __u32 tx_head;

};
#endif
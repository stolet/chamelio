#ifndef TCP_QUEUE_H_
#define TCP_QUEUE_H_

#include <linux/types.h>
#include <cham_fast.h>

#include "tcp_hdr.h"
#include "utils.h"

/* Type of queue entries */
enum tcp_queue_type {
  /* Signals that the queue is empty */
  TCP_QUEUE_EMPTY = 0,
  /* Request to slow-path to register new app */
  TCP_QUEUE_NEW_ACTX_REQ,
  /* Response from slow-path after registering app */
  TCP_QUEUE_NEW_ACTX_RES,
  /* Request to slow-path to create new socket */
  TCP_QUEUE_NEW_SOCK_REQ,
  /* Response from slow-path for created socket */
  TCP_QUEUE_NEW_SOCK_RES,
  /* Sets a new option for socket */
  TCP_QUEUE_SETOPT_REQ,
  /* Sets a new response for socket */
  TCP_QUEUE_SETOPT_RES,
  /* Bind message for a socket */
  TCP_QUEUE_BIND_REQ,
  /* Returns if bind was successful */
  TCP_QUEUE_BIND_RES,
  /* Connect request for a socket */
  TCP_QUEUE_CONNECT_REQ,
  /* Response for a connect request */
  TCP_QUEUE_CONNECT_RES,
  /* Listen request for a socket */
  TCP_QUEUE_LISTEN_REQ,
  /* Response for a listen request */
  TCP_QUEUE_LISTEN_RES,
  /* Signals that a listening socket has a new connection ready */
  TCP_QUEUE_LISTEN_NEWCONN,
  /* Accept request for a listening socket */
  TCP_QUEUE_ACCEPT_REQ,
  /* Response for an accepted socket */
  TCP_QUEUE_ACCEPT_RES,
  /* Shutdown request for a socket */
  TCP_QUEUE_SHUTDOWN_REQ,
  /* Close request for a socket */
  TCP_QUEUE_CLOSE_REQ,
  /* Bumps Chamelio when app calls send */
  TCP_QUEUE_BUMP_CHAM_TX,
  /* Bump Chamelio when app calls recv */
  TCP_QUEUE_BUMP_CHAM_RX,
  /* Signal that a TCP control packet is ready for transmission */
  TCP_QUEUE_CTL_TX,
  /* Packet data for a TCP control packet to transmit */
  TCP_QUEUE_CTL_TX_PKT,
  /* Retransmit outstanding data for a socket in the fast-path */
  TCP_QUEUE_TX_RETRANSMIT,
  /* Signal that a punted TCP control packet is ready */
  TCP_QUEUE_CTL_RX,
  /* Packet data for a punted TCP control packet */
  TCP_QUEUE_CTL_RX_PKT,
  /* Bump app when peer ACKs sent bytes */
  TCP_QUEUE_BUMP_APP_TX,
  /* Bump app when Chamelio receives a packet */
  TCP_QUEUE_BUMP_APP_RX,
};

/* Request to create new socket in slow-path */
struct tcp_queue_new_sock_req {
  /* Pointer to socket in the library */
  __u64 opaque;
} __attribute__((packed));

/* Response for new socket created */
struct tcp_queue_new_sock_res {
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
struct tcp_queue_new_actx_req {
  /* Add a byte value to request so it's not empty */
  __u8 req;
} __attribute__((packed));

/* Response when app context registers with slow-path */
struct tcp_queue_new_actx_res {
  /* Number of fast-path cores */
  __u32 n_fp_cores;
  /* Size of shm region */
  __u32 shm_len;
  /* Offset of shm region */
  __u64 shm_off;
  /* Number of elements app->slow queue */
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
  /* Size of elements app->fast queues */
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

/* Message that sends the src port and ip */
struct tcp_queue_bind_req {
  /* Socket ID used by slow-path */
  __u32 sock_id;
  /* Source port for this bind */
  __u16 local_port;
  /* Source IP for this bind */
  __u32 local_ip;
  /* Opaque pointer to struct in app library */
  __u64 opaque;
} __attribute__((packed));

/* Message that signals if bind was successful */
struct tcp_queue_bind_res {
  /* 0 for fail 1 for success */
  __u8 success;
  /* Opaque pointer to struct in app library */
  __u64 opaque;
} __attribute__((packed));

/* Message that sets an option for the socket */
struct tcp_queue_setopt_req {
  /* Type of option to set */
  __u8 opt;
  /* Socket ID to set the option */
  __u32 sock_id;
  /* Opaque pointer to socket in app library */
  __u64 opaque;
} __attribute__((packed));

/* Response that signals if setting an option was successful */
struct tcp_queue_setopt_res {
  /* 1 if request was successful 0 otherwise */
  __u8 success;
  /* Opaque pointer to socket in app library */
  __u64 opaque;
} __attribute__((packed));

/* Connect request for a TCP socket */
struct tcp_queue_connect_req {
  /* Socket ID used by slow-path */
  __u32 sock_id;
  /* Remote IP address */
  __u32 remote_ip;
  /* Remote port */
  __u16 remote_port;
  /* Opaque pointer to socket in app library */
  __u64 opaque;
} __attribute__((packed));

/* Result for a connect request */
struct tcp_queue_connect_res {
  /* 0 on success, errno on failure */
  __s32 status;
  /* Opaque pointer to socket in app library */
  __u64 opaque;
  /* Local IP address */
  __u32 local_ip;
  /* Local port */
  __u16 local_port;
  /* Remote IP address */
  __u32 remote_ip;
  /* Remote port */
  __u16 remote_port;
} __attribute__((packed));

/* Listen request for a TCP socket */
struct tcp_queue_listen_req {
  /* Socket ID used by slow-path */
  __u32 sock_id;
  /* Requested backlog */
  __u32 backlog;
  /* Opaque pointer to socket in app library */
  __u64 opaque;
} __attribute__((packed));

/* Result for a listen request */
struct tcp_queue_listen_res {
  /* 0 on success, errno on failure */
  __s32 status;
  /* Opaque pointer to socket in app library */
  __u64 opaque;
} __attribute__((packed));

/* Signals that a listener has a new established connection ready */
struct tcp_queue_listen_newconn {
  /* Opaque pointer to listening socket in app library */
  __u64 opaque;
  /* Remote IP address for the ready connection */
  __u32 remote_ip;
  /* Remote port for the ready connection */
  __u16 remote_port;
} __attribute__((packed));

/* Accept request for a TCP listener */
struct tcp_queue_accept_req {
  /* Listening socket ID used by slow-path */
  __u32 sock_id;
  /* Opaque pointer to child socket in app library */
  __u64 opaque;
} __attribute__((packed));

/* Response for an accepted connection */
struct tcp_queue_accept_res {
  /* 0 on success, errno on failure */
  __s32 status;
  /* Opaque pointer to child socket in app library */
  __u64 opaque;
  /* Opaque pointer to listening socket in app library */
  __u64 listen_opaque;
  /* ID of the child socket in slow-path */
  __u32 sock_id;
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
  /* Local IP address */
  __u32 local_ip;
  /* Local port */
  __u16 local_port;
  /* Remote IP address */
  __u32 remote_ip;
  /* Remote port */
  __u16 remote_port;
} __attribute__((packed));

/* Shutdown request for a TCP socket */
struct tcp_queue_shutdown_req {
  /* Socket ID used by slow-path */
  __u32 sock_id;
  /* Shutdown mode */
  __u8 how;
  /* Opaque pointer to socket in app library */
  __u64 opaque;
} __attribute__((packed));

/* Close request for a TCP socket */
struct tcp_queue_close_req {
  /* Socket ID used by slow-path */
  __u32 sock_id;
  /* Opaque pointer to socket in app library */
  __u64 opaque;
} __attribute__((packed));

/* Message that bumps the Chamelio TX avail */
struct tcp_queue_bump_cham_tx {
  /* Socket ID used by slow-path */
  __u32 sock_id;
  /* TX port for this bump */
  __u16 tx_port;
  /* TX IP for this bump */
  __u32 tx_ip;
  /* Bump for TX available */
  __u32 tx_avail;
} __attribute__((packed));

/* Message that bumps the Chamelio RX head */
struct tcp_queue_bump_cham_rx {
  /* Socket ID used by slow-path */
  __u32 sock_id;
  /* Bump for RX head */
  __u32 rx_head;
} __attribute__((packed));

/* Signal that a TCP control packet is ready in the paired packet queue */
struct tcp_queue_ctl_sig {
  /* Keep the signal entry non-empty */
  __u8 ready;
} __attribute__((packed));

/* Packet data for TCP control packets */
struct tcp_queue_ctl_pkt {
  /* Packet to send or process in the slow path */
  struct tcp_pkt_inner pkt;
  /* Fixed timestamp option block carried after the TCP header */
  struct tcp_timestamp_opt_pad ts_opt;
} __attribute__((packed));

/* Fast-path command that targets a socket */
struct tcp_queue_ctl_remit {
  /* Socket ID used by slow-path */
  __u32 sock_id;
} __attribute__((packed));

/* Message that bumps the app TX head after bytes are ACKed */
struct tcp_queue_bump_app_tx {
  /* Opaque pointer to struct in app library */
  __u64 opaque;
  /* Number of acknowledged TX bytes */
  __u32 tx_head;
} __attribute__((packed));

/* Message that bumps the app RX available */
struct tcp_queue_bump_app_rx {
  /* Opaque pointer to struct in app library */
  __u64 opaque;
  /* RX port for this bump */
  __u16 rx_port;
  /* RX IP for this bump */
  __u32 rx_ip;
  /* Bump for RX available */
  __u32 rx_avail;
} __attribute__((packed));

struct tcp_queue_entry {
  /* Type of queue entry. Don't update outside of enqueue or dequeue */
  volatile __u8 type;
  /* Data section of queue entry */
  union {
    struct tcp_queue_new_actx_req new_actx_req;
    struct tcp_queue_new_actx_res new_actx_res;
    struct tcp_queue_new_sock_req new_sock_req;
    struct tcp_queue_new_sock_res new_sock_res;
    struct tcp_queue_bind_req bind_req;
    struct tcp_queue_bind_res bind_res;
    struct tcp_queue_setopt_req setopt_req;
    struct tcp_queue_setopt_res setopt_res;
    struct tcp_queue_connect_req connect_req;
    struct tcp_queue_connect_res connect_res;
    struct tcp_queue_listen_req listen_req;
    struct tcp_queue_listen_res listen_res;
    struct tcp_queue_listen_newconn listen_newconn;
    struct tcp_queue_accept_req accept_req;
    struct tcp_queue_accept_res accept_res;
    struct tcp_queue_shutdown_req shutdown_req;
    struct tcp_queue_close_req close_req;
    __u8 raw[511];
  } __attribute__((packed)) data;
} __attribute__((packed));

struct tcp_queue_bump_entry {
  /* Type of queue entry. Don't update outside of enqueue or dequeue */
  volatile __u8 type;
  /* Data section of queue entry */
  union {
    struct tcp_queue_bump_app_tx bump_app_tx;
    struct tcp_queue_bump_app_rx bump_app_rx;
    struct tcp_queue_bump_cham_tx bump_cham_tx;
    struct tcp_queue_bump_cham_rx bump_cham_rx;
    /* Keeps bump queue entries compact */
    __u8 raw[31];
  } __attribute__((packed)) data;
} __attribute__((packed));

struct tcp_queue_ctl_entry {
  /* Type of queue entry. Don't update outside of enqueue or dequeue */
  volatile __u8 type;
  /* Data section of queue entry */
  union {
    struct tcp_queue_ctl_sig ctl_sig;
    struct tcp_queue_ctl_remit ctl_remit;
    /* Keeps control signal entries compact */
    __u8 raw[31];
  } __attribute__((packed)) data;
} __attribute__((packed));

struct tcp_queue_pkt_entry {
  /* Type of queue entry. Don't update outside of enqueue or dequeue */
  volatile __u8 type;
  /* Data section of queue entry */
  union {
    struct tcp_queue_ctl_pkt ctl_pkt;
    /* Keeps packet queue entries the size of a cache line */
    __u8 raw[63];
  } __attribute__((packed)) data;
} __attribute__((packed));

/* Keep queue entry sizes explicit because different queues use different layouts */
STATIC_ASSERT(sizeof(struct tcp_queue_entry) == 512, tcp_queue_entry_size);
STATIC_ASSERT(sizeof(struct tcp_queue_bump_entry) == 32, tcp_bump_queue_entry_size);
STATIC_ASSERT(sizeof(struct tcp_queue_ctl_entry) == 32, tcp_ctl_queue_entry_size);
STATIC_ASSERT(sizeof(struct tcp_queue_pkt_entry) == 64, tcp_pkt_queue_entry_size);

#endif

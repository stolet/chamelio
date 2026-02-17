#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

#include "cham_fast.h"
#include "udp_hdr.h"
#include "ip_hdr.h"
#include "eth_hdr.h"
#include "rpc.h"
#include "rpc_fast.h"
#include "rpc_queue_types.h"
#include "utils.h"
#include "rpc_hdr.h"

#define PORT_MAP 0
#define SERVER_MAP 1
#define CLIENT_MAP 2
#define WORKERS_MAP 3

/* Add these functions as helpers */
static void * (*queue_tail)(struct equeue *q) = (void *) 1001;
static int (*queue_enqueue)(struct equeue *q, __u8 type) = (void *) 1002;

static void * (*bpf_memcpy)(void *dst, void *src, size_t len) = (void *) 1003;
static void (*bpf_print)(int a) = (void *) 1004;

static __u16 (*ipv4_checksum)(void *ip_hdr) = (void *) 1005;
static __u16 (*ipv4_udptcp_cksum)(void *ip_hdr, void *udp_hdr) = (void *) 1006;

static struct cham_sched_entry * (*sched_head)(struct cham_scheduler *sched) = (void *) 1007;
static int (*sched_pop)(struct cham_scheduler *sched) = (void *) 1008;
static int (*sched_add)(struct cham_scheduler *sched, __u32 id, __u32 priority) = (void *) 1009;

static __always_inline int handle_bump_rx(struct cham_ebpf_ctx *ctx);
static __always_inline int handle_bump_tx(struct cham_ebpf_ctx *ctx);

//TODO: reduce code duplication
SEC("chamelio/event_rx")
int event_rx(struct cham_ebpf_ctx *ctx)
{ 
  int ret;
  __u32 free_bytes, tail, part;
  __u16 payload_len, ip_hdrs_len, ip_total_len; 
  __u16 udp_len, rpc_len, service;
  struct eth_hdr *eth;
  struct ip_hdr  *ip;
  struct udp_hdr *udp;
  struct rpc_hdr *rpc_hdr;
  void *payload, *pkt;
  __u16 ip_saved_chksum, ip_comp_chksum;
  __u16 udp_saved_chksum, udp_comp_chksum;
  __u8 *rx_base;
  struct rpc_server *server, *server_map;
  struct rpc_port_entry *port_map, *port_entry;
  struct rpc_worker *worker, *worker_map;
  struct equeue *q;
  struct rpc_queue_bump_entry *qe;
  struct rpc_queue_bump_app_rx *bump;
  struct rpc_client *client_map, *client;

  pkt = ctx->pkt;
  worker_map = ctx->maps[WORKERS_MAP].addr;
  server_map = ctx->maps[SERVER_MAP].addr;
  port_map = ctx->maps[PORT_MAP].addr;
  client_map = ctx->maps[CLIENT_MAP].addr;
  

  eth = (struct eth_hdr *) pkt;
  if (f_beui16(eth->type) != ETH_TYPE_IP) return -1;

  ip = (struct ip_hdr *) (pkt + sizeof(struct eth_hdr));
  if (IPH_V(ip) != 4 || IPH_HL(ip) < 5) return -1;

  ip_hdrs_len = IPH_HL(ip) * 4;
  ip_total_len = f_beui16(ip->len);

  if (ip->proto != IP_PROTO_UDP) return -1;

  if (f_beui16(ip->offset) & 0x3FFF) return -1; //fragmented packet dropped

  if (ip_total_len < ip_hdrs_len + (__u16) sizeof(struct udp_hdr))
    return -1;

  /* Verify IPv4 header checksum */
  ip_saved_chksum = ip->chksum;
  ip->chksum = 0;
  ip_comp_chksum = ipv4_checksum((void *) ip);
  ip->chksum = ip_saved_chksum;
  if (ip_comp_chksum != ip_saved_chksum)
    return -1;

  /* Parse UDP header */
  udp = (struct udp_hdr *) ((__u8 *) ip + ip_hdrs_len);
  udp_len = f_beui16(udp->len);
  if (udp_len < sizeof(struct udp_hdr))
    return -1;
  
  if (ip_total_len < ip_hdrs_len + udp_len)
    return -1;

  /* Verify UDP checksum for IPv4 (0 means “no checksum”) */
  udp_saved_chksum = udp->chksum;
  if (udp_saved_chksum != 0)
  {
    udp->chksum = 0;
    udp_comp_chksum = ipv4_udptcp_cksum((void *) ip, (void *) udp);
    udp->chksum = udp_saved_chksum;
    
    if (udp_comp_chksum != udp_saved_chksum)
    return -1;
  }

  //Parse rpc header

  rpc_hdr = (struct rpc_hdr *) ((__u8 *) udp + sizeof(struct udp_hdr));
  //size of the udp payload
  rpc_len = f_beui16(rpc_hdr->len);

  if (rpc_len < sizeof(struct rpc_hdr)) return -1;

  if (rpc_len != udp_len - sizeof(struct udp_hdr))
    return -1;
    
  payload_len = (__u16) (udp_len - sizeof(struct udp_hdr));
  payload = (__u8 *) udp + sizeof(struct udp_hdr);
  service = f_beui16(rpc_hdr->service);

  port_entry = &port_map[f_beui16(udp->dst)];
  
  if (rpc_hdr->type == 0)
  {
    //request, so it's server/worker receiving

    server = &server_map[port_entry->server_id];
    if (!server) return -1;

    //select the first worker for now
    //TODO: change in ms4
    worker = &worker_map[server->workers[0]];
    if (!worker) return -1;

    //copy payload to worker rx buffer
    rx_base = (__u8 *) ctx->shm_base + worker->rx_off;
    free_bytes = worker->rx_len - worker->rx_avail;

    if (payload_len > free_bytes) return -1;
    tail = worker->rx_head + worker->rx_avail;
    if (tail >= worker->rx_len) tail -= worker->rx_len;
    if (tail + payload_len <= worker->rx_len)
    {
      bpf_memcpy(rx_base + tail, payload, payload_len);
    }
    else
    {
      part = worker->rx_len - tail;
      bpf_memcpy(rx_base + tail, payload, part);
      bpf_memcpy(rx_base, payload + part, payload_len - part);
    }
    worker->rx_avail += payload_len;

    q = &ctx->equeues[worker->app_bump_qid].eq;
    qe = queue_tail(q);
    if (qe == NULL)
      return -1;
    bump = &qe->data.bump_app_rx;
    bump->opaque = worker->opaque;
    bump->rx_avail = payload_len;
    bump->rx_port = f_beui16(udp->src);
    bump->rx_ip = f_beui32(ip->src);

  }
  else {

    client = &client_map[port_entry->client_id];
    if (!client) return -1;

    rx_base = (__u8 *) ctx->shm_base + client->rx_off;
    free_bytes = client->rx_len - client->rx_avail;

    if (payload_len > free_bytes) return -1;
    tail = client->rx_head + client->rx_avail;
    if (tail >= client->rx_len) tail -= client->rx_len;
    if (tail + payload_len <= client->rx_len)
    {
      bpf_memcpy(rx_base + tail, payload, payload_len);
    }
    else
    {
      part = client->rx_len - tail;
      bpf_memcpy(rx_base + tail, payload, part);
      bpf_memcpy(rx_base, payload + part, payload_len - part);
    }
    client->rx_avail += payload_len;

    q = &ctx->equeues[client->app_bump_qid].eq;
    qe = queue_tail(q);
    if (qe == NULL)
      return -1;
    bump = &qe->data.bump_app_rx;
    bump->opaque = client->opaque;
    bump->rx_avail = payload_len;
    bump->rx_port = f_beui16(udp->src);
    bump->rx_ip = f_beui32(ip->src);

  }

  ret = queue_enqueue(q, RPC_QUEUE_BUMP_APP_RX);
  if (ret != 0) return -1;
  
  return 0;
}

SEC("chamelio/event_tx")
int event_tx(struct cham_ebpf_ctx *ctx)
{
  return -1;
}

SEC("chamelio/event_deq")
int event_deq(struct cham_ebpf_ctx *ctx)
{
  int ret;
  switch(ctx->qe->type) {
    case RPC_QUEUE_BUMP_CHAM_RX:
      ret = handle_bump_rx(ctx);
      break;
    case RPC_QUEUE_BUMP_CHAM_TX:
      ret = handle_bump_tx(ctx);
      break;
    default:
      ret = -1;
  }
  return ret;
}

static __always_inline int handle_bump_rx(struct cham_ebpf_ctx *ctx)
{
  __u32 new_head;
  struct rpc_queue_bump_cham_rx *bump;
  struct rpc_queue_bump_entry *qe;

  qe = (struct rpc_queue_bump_entry *) ctx->qe;
  bump = &qe->data.bump_cham_rx;

  //2 cases based on whether it's a request (worker receiving) or response (client receiving)
  if (!bump->type)
  {
    //worker receiving
    struct rpc_worker *worker_map = ctx->maps[WORKERS_MAP].addr;
    struct rpc_worker *worker = &worker_map[bump->sock_id];

    new_head = worker->rx_head + bump->rx_head;
    if (new_head >= worker->rx_len) new_head -= worker->rx_len;
    worker->rx_head = new_head;
    worker->rx_avail -= bump->rx_head;
  } 
  else
  {
    //client receiving
    struct rpc_client *client_map = ctx->maps[CLIENT_MAP].addr;
    struct rpc_client *client = &client_map[bump->sock_id];

    new_head = client->rx_head + bump->rx_head;
    if (new_head >= client->rx_len) new_head -= client->rx_len;
    client->rx_head = new_head;
    client->rx_avail -= bump->rx_head;
  }
  
  return 0;
}

//TODO: optimize the code later to reduce code duplication
static __always_inline int handle_bump_tx(struct cham_ebpf_ctx *ctx)
{ 
  int ret;
  struct rpc_queue_bump_cham_tx *bump_cham;
  struct rpc_queue_bump_entry *qe;
  struct equeue *q;
  struct rpc_queue_bump_app_tx *bump_app;
  struct rpc_client *client, *client_map;
  struct rpc_server *server, *server_map;
  struct rpc_worker *worker, *worker_map;
  __u16 payload_len, pkt_hdrs_len, ip_hdr_len, udp_hdr_len, eth_hdr_len;
  __u32 new_head;
  __u64 part;
  void *payload;
  struct rpc_pkt *p = (struct rpc_pkt *) ctx->pkt;

  qe = (struct rpc_queue_bump_entry *) ctx->qe;
  bump_cham = &qe->data.bump_cham_tx;
  payload_len = bump_cham->tx_avail;

  //2 cases based on whether it's a request (worker sending) or response (client sending)
  if (!bump_cham->type)
  {
    //client sending

    client_map = ctx->maps[CLIENT_MAP].addr;
    client = &client_map[bump_cham->sock_id];
    
    //rpc hdr + data alr in the tx buffer
    if (payload_len > UDP_MSS) 
      payload_len = UDP_MSS;

    eth_hdr_len = sizeof(struct eth_hdr);
    ip_hdr_len = sizeof(struct ip_hdr);
    udp_hdr_len = sizeof(struct udp_hdr);

    pkt_hdrs_len = eth_hdr_len + ip_hdr_len + udp_hdr_len; 

    //set the eth, ip and udp header + rpc header in the udp payload
    p->eth.type = t_beui16(ETH_TYPE_IP);

    IPH_VHL_SET(&p->ip, 4, 5);
    p->ip._tos = 0;
    p->ip.len = t_beui16(sizeof(struct ip_hdr) + sizeof(struct udp_hdr) + payload_len);
    p->ip.id = t_beui16(3);
    p->ip.offset = t_beui16(0);
    p->ip.ttl = 0xff;
    p->ip.proto = IP_PROTO_UDP;
    p->ip.src = t_beui32(client->local_ip);
    p->ip.dst = t_beui32(bump_cham->tx_ip);
    p->ip.chksum = 0;

    p->udp.src = t_beui16(client->local_port);
    p->udp.dst = t_beui16(bump_cham->tx_port);
    p->udp.len = t_beui16(sizeof(struct udp_hdr) + payload_len);

    payload = ctx->pkt + pkt_hdrs_len;
    if (client->tx_head + payload_len <= client->tx_len) 
    {
      bpf_memcpy(payload, ctx->shm_base + client->tx_off + client->tx_head, payload_len);
    } 
    else 
    {
      part = client->tx_len - client->tx_head;
      bpf_memcpy(payload, ctx->shm_base + client->tx_off + client->tx_head, part);
      bpf_memcpy(payload + part, ctx->shm_base + client->tx_off, payload_len - part);
    }

    /* Compute checksums */
    p->udp.chksum = 0;
    p->udp.chksum = ipv4_udptcp_cksum((void *) &p->ip, (void *) &p->udp);
    p->ip.chksum = ipv4_checksum((void *) &p->ip);

    new_head = client->tx_head + bump_cham->tx_avail;
    if (new_head >= client->tx_len) new_head -= client->tx_len;
    client->tx_head = new_head;
    client->tx_avail -= payload_len;
    q = &ctx->equeues[client->app_bump_qid].eq;
  }
  else 
  {
    //worker sending
    worker_map = ctx->maps[WORKERS_MAP].addr;
    worker = &worker_map[bump_cham->sock_id];

    //rpc hdr + data alr in the tx buffer
    if (payload_len > UDP_MSS) 
      payload_len = UDP_MSS;

    pkt_hdrs_len = sizeof(struct eth_hdr) + sizeof(struct ip_hdr) + 
      sizeof(struct udp_hdr); 

    //find the server of the worker to find the local port/ip
    server_map = ctx->maps[SERVER_MAP].addr;
    server = &server_map[worker->server_id];

    //set the eth, ip and udp header + rpc header in the udp payload
    p->eth.type = t_beui16(ETH_TYPE_IP);

    IPH_VHL_SET(&p->ip, 4, 5);
    p->ip._tos = 0;
    p->ip.len = t_beui16(sizeof(struct ip_hdr) + sizeof(struct udp_hdr) + payload_len);
    p->ip.id = t_beui16(3);
    p->ip.offset = t_beui16(0);
    p->ip.ttl = 0xff;
    p->ip.proto = IP_PROTO_UDP;
    p->ip.src = t_beui32(server->local_ip);
    p->ip.dst = t_beui32(bump_cham->tx_ip);
    p->ip.chksum = 0;

    p->udp.src = t_beui16(server->local_port);
    p->udp.dst = t_beui16(bump_cham->tx_port);
    p->udp.len = t_beui16(sizeof(struct udp_hdr) + payload_len);

    payload = ctx->pkt + pkt_hdrs_len;
    if (worker->tx_head + payload_len <= worker->tx_len) 
    {
      bpf_memcpy(payload, ctx->shm_base + worker->tx_off + worker->tx_head, payload_len);
    } 
    else 
    {
      part = worker->tx_len - worker->tx_head;
      bpf_memcpy(payload, ctx->shm_base + worker->tx_off + worker->tx_head, part);
      bpf_memcpy(payload + part, ctx->shm_base + worker->tx_off, payload_len - part);
    }

    /* Compute checksums */
    p->udp.chksum = 0;
    p->udp.chksum = ipv4_udptcp_cksum((void *) &p->ip, (void *) &p->udp);
    p->ip.chksum = ipv4_checksum((void *) &p->ip);

    new_head = worker->tx_head + bump_cham->tx_avail;
    if (new_head >= worker->tx_len) new_head -= worker->tx_len;
    worker->tx_head = new_head;
    worker->tx_avail -= payload_len;
    q = &ctx->equeues[worker->app_bump_qid].eq;
  }
  
  qe = (struct rpc_queue_bump_entry *) queue_tail(q);
  if (qe == NULL)
    return -1;

  bump_app = &qe->data.bump_app_tx;
  if (!bump_cham->type)
  {
    //client sending
    bump_app->opaque = client->opaque; //request
  } 
  else 
  {
    //worker sending
    bump_app->opaque = worker->opaque; //response
  }
  
  bump_app->tx_head = payload_len;
  ret = queue_enqueue(q, RPC_QUEUE_BUMP_APP_TX);
  if (ret != 0) return -1;

  return pkt_hdrs_len + payload_len;
}

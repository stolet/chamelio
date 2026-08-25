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

#define CLIENT_MAP 0
#define SERVER_MAP 1
#define WORKERS_MAP 2
#define PORT_MAP 3
#define CFG_MAP 4

#define RPC_MAX_PAYLOAD (FAST_L3_PKT_ROOM - sizeof(struct rpc_pkt_inner))

// Atomic read/write helpers for x86 host only
#define READ_ONCE(ptr) (*(volatile typeof(ptr) *)&(ptr))
#define WRITE_ONCE(ptr, val) (*(volatile typeof(ptr) *)&(ptr) = (val))
#define COMPILER_BARRIER() __asm__ __volatile__("" ::: "memory")

/* Add these functions as helpers */
static void *(*ebpf_queue_tail)(struct equeue *q, __u64 elsize) = (void *)1001;
static int (*queue_enqueue)(struct equeue *q, __u8 type) = (void *)1002;

static void *(*ebpf_memcpy)(void *dst, void *src, size_t len) = (void *)1003;
static void (*ebpf_print)(int a) = (void *)1004;

static __u16 (*ebpf_ipv4_checksum)(void *ip_hdr) = (void *)1005;
static __u16 (*ebpf_ipv4_udptcp_cksum)(void *ip_hdr, void *udp_hdr) = (void *)1006;

static struct cham_sched_entry *(*sched_head)(struct cham_scheduler *sched) = (void *)1007;
static int (*sched_pop)(struct cham_scheduler *sched) = (void *)1008;
static int (*sched_add)(struct cham_scheduler *sched, __u32 id, __u32 priority) = (void *)1009;
static void * (*ebpf_map_get)(void *map_base, __u32 len) = (void *) 1010;
static void * (*ebpf_map_lookup)(void *map_base, __u64 id, __u64 elsize) = (void *) 1011;

static __always_inline int handle_bump_rx(struct cham_ebpf_ctx *ctx);
static __always_inline int handle_bump_tx(struct cham_ebpf_ctx *ctx);
static __always_inline __u16 find_free_port(struct cham_ebpf_ctx *ctx);

// TODO: reduce code duplication
SEC("chamelio/event_rx")
int event_rx(struct cham_ebpf_ctx *ctx)
{
  int ret, i;
  __u32 free_bytes, used_bytes, head, tail, part;
  __u16 payload_len, ip_hdrs_len, ip_total_len;
  __u16 udp_len, rpc_len, service;
  struct eth_hdr *eth;
  struct ip_hdr *ip;
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
  __u32 best_worker_id, fewest_jobs, worker_id;
  struct rpc_rx_meta meta;

  pkt = ctx->pkt;
  worker_map = ctx->maps[WORKERS_MAP].addr;
  server_map = ctx->maps[SERVER_MAP].addr;
  port_map = ctx->maps[PORT_MAP].addr;
  client_map = ctx->maps[CLIENT_MAP].addr;
  eth = (struct eth_hdr *)pkt;

  /* Parse IP hdr */
  if (ctx->pkt + sizeof(struct ip_hdr) > ctx->pkt_end)
    return -1;

  // if (f_beui16(eth->type) != ETH_TYPE_IP)
  //   return -1;

  // ip = (struct ip_hdr *)(pkt + sizeof(struct eth_hdr));

  ip = (struct ip_hdr *) ctx->pkt;
  if (IPH_V(ip) != 4 || IPH_HL(ip) < 5) return -1;

  if (ip->proto != IP_PROTO_UDP)
    return -1;

  if (f_beui16(ip->offset) & 0x3FFF)
    return -1; // fragmented packet dropped

  ip_hdrs_len = (__u16) IPH_HL(ip) * 4;
  ip_total_len = f_beui16(ip->len);

  if (ip_total_len < ip_hdrs_len + (__u16)sizeof(struct udp_hdr))
    return -1;

  /* Verify IPv4 header checksum */
  // ip_saved_chksum = ip->chksum;
  // ip->chksum = 0;
  // ip_comp_chksum = ipv4_checksum((void *)ip);
  // ip->chksum = ip_saved_chksum;
  // if (ip_comp_chksum != ip_saved_chksum)
  //   return -1;

  /* Parse UDP header */
  if ((__u8 *)ip + ip_hdrs_len + sizeof(struct udp_hdr) > 
      (__u8 *)ctx->pkt_end)
    return -1;

  udp = (struct udp_hdr *)((__u8 *)ip + ip_hdrs_len);
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
    udp_comp_chksum = ebpf_ipv4_udptcp_cksum((void *)ip, (void *)udp);
    udp->chksum = udp_saved_chksum;

    if (udp_comp_chksum != udp_saved_chksum)
      return -1;
  }

  // Parse rpc header
  rpc_hdr = (struct rpc_hdr *)((__u8 *)udp + sizeof(struct udp_hdr));
  if ((__u8 *)rpc_hdr + sizeof(struct rpc_hdr) > 
      (__u8 *)ctx->pkt_end)
    return -1;

  // size of the udp payload
  rpc_len = f_beui16(rpc_hdr->len);

  if (rpc_len < sizeof(struct rpc_hdr))
    return -1;

  if (rpc_len != udp_len - sizeof(struct udp_hdr))
    return -1;

  payload_len = (__u16)(udp_len - sizeof(struct udp_hdr));
  payload = (__u8 *)udp + sizeof(struct udp_hdr);
  service = f_beui16(rpc_hdr->service);

  // port_entry = &port_map[f_beui16(udp->dst)];
  port_entry = ebpf_map_lookup(ctx->maps[PORT_MAP].addr,
    f_beui16(udp->dst), sizeof(struct rpc_port_entry));

  if (port_entry == NULL)
    return -1;

  if (rpc_hdr->type == 0)
  {
    // request, so it's server/worker receiving
    if (port_entry->server_id == (__u32)INVALID_ID)
      return -1;

    server = ebpf_map_lookup(ctx->maps[SERVER_MAP].addr,
      port_entry->server_id, sizeof(struct rpc_server));

    if (server == NULL)
      return -1;

    // server = &server_map[port_entry->server_id];
    if (server->n_workers == 0)
      return -1;

    if (server->n_workers > MAX_WORKERS)
      return -1;

    if (service >= MAX_SERVICE_NUMBER || !server->service_table[service])
      return -1;

    if (server->app_lb_mode)
    {
      /* write entry to server's shared RX ring. */
      __u32 entry_len = (__u32)sizeof(struct rpc_rx_meta) + payload_len;
      
      // rx_base = (__u8 *)ctx->shm_base + server->rx_off;
      rx_base = ebpf_map_get(ctx->shm_base + server->rx_off,
                server->rx_len);
      if (rx_base == NULL)
        return -1;

      // head = server->rx_head;
      // tail = server->rx_tail;
      
      head = READ_ONCE(server->rx_head);
      COMPILER_BARRIER();
      // modified only by the fast path, so relaxed is fine
      tail = READ_ONCE(server->rx_tail);

      if (tail >= head)
        used_bytes = tail - head;
      else
        used_bytes = server->rx_len - head + tail;

      /* Keep one byte free so rx_head == rx_tail always means empty. */
      free_bytes = server->rx_len - used_bytes - 1;
      if (entry_len > free_bytes)
        return -1;

      meta.rx_ip   = f_beui32(ip->src);
      meta.rx_port = f_beui16(udp->src);
      meta._pad    = 0;

      if (sizeof(meta) <= server->rx_len - tail)
      {
        ebpf_memcpy(rx_base + tail, &meta, sizeof(meta));
      }
      else
      {
        part = server->rx_len - tail;
        ebpf_memcpy(rx_base + tail, &meta, part);
        ebpf_memcpy(rx_base, (__u8 *)(&meta) + part, sizeof(meta) - part);
      }
      tail += (__u32)sizeof(struct rpc_rx_meta);
      if (tail >= server->rx_len)
        tail -= server->rx_len;

      if (tail > server->rx_len)
        return -1;

      if (payload_len <= server->rx_len - tail)
      {
        ebpf_memcpy(rx_base + tail, payload, payload_len);
      }
      else
      {
        part = server->rx_len - tail;
        ebpf_memcpy(rx_base + tail, payload, part);
        ebpf_memcpy(rx_base, payload + part, payload_len - part);
      }
      tail += payload_len;
      if (tail >= server->rx_len)
        tail -= server->rx_len;
      // server->rx_tail = tail;
      COMPILER_BARRIER();
      WRITE_ONCE(server->rx_tail, tail);
      return 0;
    }

    best_worker_id = (__u32)INVALID_ID;

    if (server->ebpf_lb_mode == 1)
    {
      /* Round-robin: start from rr_next, skip workers whose buffer is full */
      __u32 rr_start = server->rr_next % server->n_workers;
#pragma unroll
      for (i = 0; i < MAX_WORKERS; i++)
      {
        __u32 idx;
        if (i >= server->n_workers)
          break;
        //use subtraction arithmetic here 
        idx = (rr_start + (__u32)i) % server->n_workers;
        if (idx >= MAX_WORKERS)
          continue;
        worker_id = server->workers[idx];
        if (worker_id == (__u32)INVALID_ID)
          continue;

        worker = ebpf_map_lookup(worker_map, worker_id,
                sizeof(struct rpc_worker));
        if (worker == NULL)
          continue;
        
        free_bytes = worker->rx_len - worker->rx_avail;
        if (payload_len > free_bytes)
          continue;
        best_worker_id = worker_id;
        server->rr_next = (idx + 1) % server->n_workers;
        break;
      }
    }
    else if (server->ebpf_lb_mode == 2)
    {
      /* LWL: pick worker with least total remaining work (cost-weighted) */
      __u32 min_work = (__u32)-1;
      __u32 req_cost = (service < MAX_SERVICE_NUMBER)
                       ? server->service_cost[service] : 1;
#pragma unroll
      for (i = 0; i < MAX_WORKERS; i++)
      {
        if (i >= server->n_workers)
          break;
        worker_id = server->workers[i];
        if (worker_id == (__u32)INVALID_ID)
          continue;
        
        // worker = &worker_map[worker_id];
        
        worker = ebpf_map_lookup(worker_map, worker_id,
                sizeof(struct rpc_worker));
        if (worker == NULL)
          continue;

        free_bytes = worker->rx_len - worker->rx_avail;
        if (payload_len > free_bytes)
          continue;
        if (best_worker_id == (__u32)INVALID_ID ||
            worker->work_remaining < min_work)
        {
          best_worker_id = worker_id;
          min_work = worker->work_remaining;
        }
      }
      /* Charge the cost to the chosen worker. */
      if (best_worker_id != (__u32)INVALID_ID)
      {
        worker = ebpf_map_lookup(worker_map, best_worker_id,
                    sizeof(struct rpc_worker));
        if (worker == NULL)
          return -1;
        __sync_fetch_and_add(&worker->work_remaining, req_cost);
      }
    }
    else
    {
      /* eBPF JSQ: pick worker with fewest pending jobs */
      fewest_jobs = (__u32)-1;
#pragma unroll
      for (i = 0; i < MAX_WORKERS; i++)
      {
        if (i >= server->n_workers)
          break;

        worker_id = server->workers[i];
        if (worker_id == (__u32)INVALID_ID)
          continue;

        // worker = &worker_map[worker_id];

        worker = ebpf_map_lookup(worker_map, worker_id,
                sizeof(struct rpc_worker));

        if (worker == NULL)
          continue;

        free_bytes = worker->rx_len - worker->rx_avail;
        if (payload_len > free_bytes)
          continue;

        if (best_worker_id == (__u32)INVALID_ID ||
            worker->jobs_pending < fewest_jobs)
        {
          best_worker_id = worker_id;
          fewest_jobs = worker->jobs_pending;
        }
      }
    }

    if (best_worker_id == (__u32)INVALID_ID)
      return -1;

    // worker = &worker_map[best_worker_id];

    worker = ebpf_map_lookup(worker_map, best_worker_id,
                sizeof(struct rpc_worker));
    if (worker == NULL)
      return -1;

    // copy payload to worker rx buffer
    // rx_base = (__u8 *)ctx->shm_base + worker->rx_off;

    rx_base = ebpf_map_get(ctx->shm_base + worker->rx_off,
                worker->rx_len);
    if (rx_base == NULL)
      return -1;
    
    free_bytes = worker->rx_len - worker->rx_avail;

    if (payload_len > free_bytes)
      return -1;
    tail = worker->rx_head + worker->rx_avail;
    if (tail >= worker->rx_len)
      tail -= worker->rx_len;

    if (tail >= worker->rx_len)
      return -1;

    if (payload_len <= worker->rx_len - tail)
    {
      ebpf_memcpy(rx_base + tail, payload, payload_len);
    }
    else
    {
      part = worker->rx_len - tail;
      ebpf_memcpy(rx_base + tail, payload, part);
      ebpf_memcpy(rx_base, payload + part, payload_len - part);
    }
    worker->rx_avail += payload_len;
    __sync_fetch_and_add(&worker->jobs_pending, 1);

    if (worker->app_bump_qid >= MAX_PROTO_QUEUES)
      return -1;
    q = &ctx->equeues[worker->app_bump_qid].eq;
    qe = ebpf_queue_tail(q, sizeof(struct rpc_queue_bump_entry));
    if (qe == NULL)
      return -1;
    bump = &qe->data.bump_app_rx;
    bump->opaque = worker->opaque;
    bump->rx_avail = payload_len;
    bump->rx_port = f_beui16(udp->src);
    bump->rx_ip = f_beui32(ip->src);
    bump->type = 0; // request
  }
  else
  {
    if (port_entry->client_id == (__u32)INVALID_ID)
      return -1;

    client = ebpf_map_lookup(ctx->maps[CLIENT_MAP].addr,
      port_entry->client_id, sizeof(struct rpc_client));
    if (client == NULL)
      return -1;

    // rx_base = (__u8 *)ctx->shm_base + client->rx_off;
    rx_base = ebpf_map_get(ctx->shm_base + client->rx_off,
                client->rx_len);
    if (rx_base == NULL)
      return -1;
    
    free_bytes = client->rx_len - client->rx_avail;

    if (payload_len > free_bytes)
      return -1;
    tail = client->rx_head + client->rx_avail;
    if (tail >= client->rx_len)
      tail -= client->rx_len;
    if (payload_len <= client->rx_len - tail)
    {
      ebpf_memcpy(rx_base + tail, payload, payload_len);
    }
    else
    {
      part = client->rx_len - tail;
      ebpf_memcpy(rx_base + tail, payload, part);
      ebpf_memcpy(rx_base, payload + part, payload_len - part);
    }
    client->rx_avail += payload_len;

    if (client->app_bump_qid >= MAX_PROTO_QUEUES)
      return -1;
    q = &ctx->equeues[client->app_bump_qid].eq;
    qe = ebpf_queue_tail(q, sizeof(struct rpc_queue_bump_entry));
    if (qe == NULL)
      return -1;
    bump = &qe->data.bump_app_rx;
    bump->opaque = client->opaque;
    bump->rx_avail = payload_len;
    bump->rx_port = f_beui16(udp->src);
    bump->rx_ip = f_beui32(ip->src);
    bump->type = 1; // response
  }

  ret = queue_enqueue(q, RPC_QUEUE_BUMP_APP_RX);
  if (ret != 0)
    return -1;

  return 0;
}


SEC("chamelio/event_sched")
int event_sched(struct cham_ebpf_ctx *ctx)
{
  return -1;
}

SEC("chamelio/event_deq")
int event_deq(struct cham_ebpf_ctx *ctx)
{
  int ret;

  if ((__u8 *)ctx->qe + sizeof(*ctx->qe) > (__u8 *)ctx->shm_end)
    return -1;
  switch (ctx->qe->type)
  {
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

  qe = (struct rpc_queue_bump_entry *)ctx->qe;
  bump = &qe->data.bump_cham_rx;

  // 2 cases based on whether it's a request (worker receiving) or response (client receiving)
  if (!bump->type)
  {
    // worker receiving
    struct rpc_worker *worker_map = ctx->maps[WORKERS_MAP].addr;
    if (bump->sock_id >= MAX_WORKERS)
      return -1;
    struct rpc_worker *worker = ebpf_map_lookup(worker_map, bump->sock_id,
        sizeof(struct rpc_worker));
    if (worker == NULL)
      return -1;

    new_head = worker->rx_head + bump->rx_head;
    if (new_head >= worker->rx_len)
      new_head -= worker->rx_len;
    worker->rx_head = new_head;
    worker->rx_avail -= bump->rx_head;
  }
  else
  {
    // client receiving
    struct rpc_client *client_map = ctx->maps[CLIENT_MAP].addr;
    if (bump->sock_id >= MAX_CLIENTS)
      return -1;
    struct rpc_client *client = ebpf_map_lookup(client_map, bump->sock_id,
        sizeof(struct rpc_client));
    if (client == NULL)
      return -1;

    new_head = client->rx_head + bump->rx_head;
    if (new_head >= client->rx_len)
      new_head -= client->rx_len;
    client->rx_head = new_head;
    client->rx_avail -= bump->rx_head;
  }

  return 0;
}

static __always_inline __u16 find_free_port(struct cham_ebpf_ctx *ctx)
{
  __u16 i, next_port;
  __u32 nr;
  struct rpc_cfg *cfg;
  struct rpc_port_entry *port;
  struct cham_map *map;

  map = &ctx->maps[CFG_MAP];
  cfg = ebpf_map_lookup(map->addr, 0, sizeof(struct rpc_cfg));
  if (cfg == NULL)
    return 0;

  next_port = cfg->next_port;
  if (next_port < MIN_PORT || next_port > MAX_PORT)
    next_port = MIN_PORT;

  map = &ctx->maps[PORT_MAP];
#pragma unroll
  for (i = 0; i < RPC_PORT_SCAN_MAX; i++)
  {
    nr = next_port + i;
    if (nr > MAX_PORT)
      nr = MIN_PORT + nr - MAX_PORT - 1;

    port = ebpf_map_lookup(map->addr, nr, sizeof(struct rpc_port_entry));
    if (port == NULL)
      return 0;

    if (port->server_id == (__u32)INVALID_ID &&
        port->client_id == (__u32)INVALID_ID)
    {
      cfg->next_port = nr + 1;
      if (cfg->next_port > MAX_PORT)
        cfg->next_port = MIN_PORT;
      return (__u16)nr;
    }
  }
  return 0;
}

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
  struct rpc_port_entry *ports;
  __u16 free_port;
  __u16 payload_len, pkt_hdrs_len, ip_hdr_len, udp_hdr_len;
  __u32 tx_off, tx_head, tx_len, new_head, app_bump_qid;
  __u32 max_payload;
  __u64 part, opaque;
  void *payload;

  struct rpc_pkt_inner *p = (struct rpc_pkt_inner *)ctx->pkt;
  if ((__u8 *)p + sizeof(struct rpc_pkt_inner) > (__u8 *)ctx->pkt_end)
    return -1;

  // struct rpc_pkt *p = (struct rpc_pkt *)ctx->pkt;
  // if ((__u8 *)p + sizeof(struct rpc_pkt) > (__u8 *)ctx->pkt_end)
  //   return -1;

  qe = (struct rpc_queue_bump_entry *)ctx->qe;
  bump_cham = &qe->data.bump_cham_tx;

  /* Calculate number of bytes to transmit */
  payload_len = bump_cham->tx_avail;
  max_payload = (__u32) ((__u8 *) ctx->pkt_end - 
      ((__u8 *)p + sizeof(struct rpc_pkt_inner)));
  if (max_payload > RPC_MAX_PAYLOAD)
    payload_len = RPC_MAX_PAYLOAD;
  if (payload_len > max_payload)
    payload_len = max_payload;

  /* Drop if payload len out of bounds */
  if ((__u8 *)p + sizeof(struct rpc_pkt_inner) + payload_len > 
      (__u8 *)ctx->pkt_end)
    return -1;

  // rpc hdr + data alr in the tx buffer
  if (payload_len > UDP_MSS + sizeof(struct rpc_hdr))
    payload_len = UDP_MSS + sizeof(struct rpc_hdr);

  udp_hdr_len = sizeof(struct udp_hdr);
  ip_hdr_len = sizeof(struct ip_hdr);
  pkt_hdrs_len = ip_hdr_len + udp_hdr_len;

  // set hdrs
  // p->eth.type = t_beui16(ETH_TYPE_IP);
  //set ip hdr
  IPH_VHL_SET(&p->ip, 4, 5);
  p->ip._tos = 0;
  p->ip.len = t_beui16(sizeof(struct ip_hdr) + sizeof(struct udp_hdr) + payload_len);
  p->ip.id = t_beui16(3);
  p->ip.offset = t_beui16(0);
  p->ip.ttl = 0xff;
  p->ip.proto = IP_PROTO_UDP;
  // src for both ip and udp set inside if/else
  p->ip.dst = t_beui32(bump_cham->tx_ip);
  p->ip.chksum = 0;

  //set udp hdr
  p->udp.dst = t_beui16(bump_cham->tx_port);
  p->udp.len = t_beui16(udp_hdr_len + payload_len);

  /* Copy data to pkt */
  payload = ctx->pkt + pkt_hdrs_len;

  // is_client = !bump_cham->type;
  if (!bump_cham->type)
  {
    // client sending
    client_map = ctx->maps[CLIENT_MAP].addr;
    if (bump_cham->sock_id >= MAX_CLIENTS)
      return -1;
    client = ebpf_map_lookup(client_map, bump_cham->sock_id,
        sizeof(struct rpc_client));
    if (client == NULL)
      return -1;

    // Set port if local_port == 0
    if (client->local_port == 0)
    {
      free_port = find_free_port(ctx);
      if (free_port == 0)
        return -1;
      ports = ebpf_map_lookup(ctx->maps[PORT_MAP].addr, free_port,
          sizeof(struct rpc_port_entry));
      if (ports == NULL)
        return -1;
      ports->client_id = client->id;
      client->local_port = free_port;
    }

    p->ip.src = t_beui32(client->local_ip);
    p->udp.src = t_beui16(client->local_port);

    tx_off = client->tx_off;
    tx_head = client->tx_head;
    tx_len = client->tx_len;

    app_bump_qid = client->app_bump_qid;

    opaque = client->opaque;
  }
  else
  {
    // worker sending
    worker_map = ctx->maps[WORKERS_MAP].addr;
    if (bump_cham->sock_id >= MAX_WORKERS)
      return -1;
    worker = ebpf_map_lookup(worker_map, bump_cham->sock_id,
        sizeof(struct rpc_worker));
    if (worker == NULL)
      return -1;

    // find the server of the worker to find the local port/ip
    server_map = ctx->maps[SERVER_MAP].addr;
    server = ebpf_map_lookup(server_map, worker->server_id,
        sizeof(struct rpc_server));
    if (server == NULL)
      return -1;

    p->ip.src = t_beui32(server->local_ip);
    p->udp.src = t_beui16(server->local_port);

    tx_off = worker->tx_off;
    tx_head = worker->tx_head;
    tx_len = worker->tx_len;
    app_bump_qid = worker->app_bump_qid;
    opaque = worker->opaque;
  }

  if (tx_head + payload_len <= tx_len)
  {
    ebpf_memcpy(payload, ctx->shm_base + tx_off + tx_head, payload_len);
  }
  else
  {
    part = tx_len - tx_head;
    ebpf_memcpy(payload, ctx->shm_base + tx_off + tx_head, part);
    ebpf_memcpy(payload + part, ctx->shm_base + tx_off, payload_len - part);
  }

  /* Compute checksums */
  p->udp.chksum = 0;
  p->udp.chksum = ebpf_ipv4_udptcp_cksum((void *)&p->ip, (void *)&p->udp);
  p->ip.chksum = ebpf_ipv4_checksum((void *)&p->ip);

  new_head = tx_head + payload_len;
  if (new_head >= tx_len)
    new_head -= tx_len;

  if (!bump_cham->type)
  {
    if (bump_cham->sock_id >= MAX_CLIENTS)
      return -1;
    client = ebpf_map_lookup(ctx->maps[CLIENT_MAP].addr,
        bump_cham->sock_id, sizeof(struct rpc_client));
    if (client == NULL)
      return -1;

    client->tx_head = new_head;
    client->tx_avail -= payload_len;
  }
  else
  {
    if (bump_cham->sock_id >= MAX_WORKERS)
      return -1;
    worker = ebpf_map_lookup(ctx->maps[WORKERS_MAP].addr,
        bump_cham->sock_id, sizeof(struct rpc_worker));
    if (worker == NULL)
      return -1;

    worker->tx_head = new_head;
    worker->tx_avail -= payload_len;
  }

  // send a bump to application
  if (app_bump_qid >= MAX_PROTO_QUEUES)
    return -1;
  q = &ctx->equeues[app_bump_qid].eq;

  qe = (struct rpc_queue_bump_entry *)ebpf_queue_tail(q,
      sizeof(struct rpc_queue_bump_entry));
  if (qe == NULL)
    return -1;

  bump_app = &qe->data.bump_app_tx;
  bump_app->opaque = opaque;

  bump_app->tx_head = payload_len;
  bump_app->type = bump_cham->type;
  ret = queue_enqueue(q, RPC_QUEUE_BUMP_APP_TX);
  if (ret != 0)
    return -1;

  return pkt_hdrs_len + payload_len;
}

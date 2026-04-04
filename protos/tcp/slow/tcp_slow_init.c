#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <cham_lib.h>
#include <unistd.h>

#include "tcp_slow_internal.h"
#include "clock.h"
#include "queue_fns.h"
#include "log.h"
#include "tcp_config.h"
#include "tomgr.h"

int tcp_slow_init(struct tcp_slow_context *ctx)
{
  int fd, ret, i, j;
  struct stat statbuf;
  struct proto_ebpf_lib *ebpf;
  __u8 *ebpf_bytecode;
  struct guest_lib *g;
  struct proto_lib *p;
  struct proto_map_lib *port_map, *socks_map, *flow_map, *ctrl_map;
  struct proto_queue_lib *protoq;
  __u16 fast_slow_sig_qid;
  __u16 fast_slow_pkt_qid;
  __u16 slow_fast_sig_qid;
  __u16 slow_fast_pkt_qid;
  struct tcp_port *ports;
  struct tcp_flow_bucket *flows;
  struct tcp_ctrl_cfg *cfg;
  struct tcp_listener_slow *listeners;
  struct tcp_sock_meta_slow *sock_meta;
  struct tcp_port *bound_ports;
  struct tomgr *tomgr;

  tomgr = tomgr_init();
  if (tomgr == NULL)
  {
    LOG_ERROR("failed to allocate timeout manager");
    abort();
  }

  ret = clock_calibrate_tsc();
  if (ret != 0)
  {
    LOG_ERROR("failed to calibrate tsc");
    abort();
  }

  if (!ctx->config.virt)
  {
    g = cham_connect_guest();
    if (g == NULL)
    {
      LOG_ERROR("TCP slow-path couldn't connect to Chamelio");
      abort();
    }

    p = cham_new_proto_bare(g);
    if (p == NULL)
    {
      LOG_ERROR("TCP slow-path failed to register protocol with Chamelio");
      abort();
    }
  }
  else
  {
    p = cham_new_proto_virt();
    if (p == NULL)
    {
      LOG_ERROR("TCP slow-path failed to register protocol with Chamelio");
      abort();
    }
  }

  fd = open(TCP_EBPF_BYTECODE, O_RDWR);
  if (fd < 0)
  {
    LOG_ERROR("Failed to open TCP eBPF bytecode");
    abort();
  }
  fstat(fd, &statbuf);

  ebpf = cham_allocate_ebpf(p, statbuf.st_size);
  if (ebpf == NULL)
  {
    LOG_ERROR("Failed to allocate space for eBPF bytecode in shared memory");
    abort();
  }

  ebpf_bytecode = mmap(NULL, ebpf->size, PROT_READ | PROT_EXEC,
      MAP_PRIVATE, fd, 0);
  if (ebpf_bytecode == NULL)
  {
    LOG_ERROR("failed to mmap eBPF bytecode");
    abort();
  }

  ret = cham_upload_ebpf(p, ebpf_bytecode, ebpf->size);
  if (ret != 0)
  {
    LOG_ERROR("failed to upload eBPF bytecode to shared memory");
    abort();
  }

  port_map = cham_new_map(p, MAX_PORT + 1, sizeof(struct tcp_port));
  if (port_map == NULL)
  {
    LOG_ERROR("failed to create map for listener port lookup");
    abort();
  }

  socks_map = cham_new_map(p, MAX_SOCKETS, sizeof(struct tcp_sock));
  if (socks_map == NULL)
  {
    LOG_ERROR("failed to create map to hold sockets");
    abort();
  }

  flow_map = cham_new_map(p, TCP_FLOW_BUCKETS, sizeof(struct tcp_flow_bucket));
  if (flow_map == NULL)
  {
    LOG_ERROR("failed to create map for TCP flow lookup");
    abort();
  }

  ctrl_map = cham_new_map(p, 1, sizeof(struct tcp_ctrl_cfg));
  if (ctrl_map == NULL)
  {
    LOG_ERROR("failed to create map for TCP control configuration");
    abort();
  }

  protoq = cham_new_queue(p, ctx->config.ctrlq_len,
      sizeof(struct tcp_queue_bump_entry));
  if (protoq == NULL)
  {
    LOG_ERROR("failed to create fast->slow control signal queue");
    abort();
  }
  ctx->fast_slow_sig_q = dqueue_new(protoq->nelems, protoq->elsize,
      p->shm_base + protoq->off, protoq->off);
  if (ctx->fast_slow_sig_q == NULL)
  {
    LOG_ERROR("failed to create dqueue for fast->slow control signal queue");
    abort();
  }
  fast_slow_sig_qid = protoq->id;

  protoq = cham_new_queue(p, ctx->config.ctrlq_len,
      sizeof(struct tcp_queue_bump_entry));
  if (protoq == NULL)
  {
    LOG_ERROR("failed to create fast->slow control packet queue");
    abort();
  }
  ctx->fast_slow_pkt_q = dqueue_new(protoq->nelems, protoq->elsize,
      p->shm_base + protoq->off, protoq->off);
  if (ctx->fast_slow_pkt_q == NULL)
  {
    LOG_ERROR("failed to create dqueue for fast->slow control packet queue");
    abort();
  }
  fast_slow_pkt_qid = protoq->id;

  protoq = cham_new_queue(p, ctx->config.ctrlq_len,
      sizeof(struct tcp_queue_bump_entry));
  if (protoq == NULL)
  {
    LOG_ERROR("failed to create slow->fast control signal queue");
    abort();
  }
  ctx->slow_fast_sig_q = equeue_new(protoq->nelems, protoq->elsize,
      p->shm_base + protoq->off, protoq->off);
  if (ctx->slow_fast_sig_q == NULL)
  {
    LOG_ERROR("failed to create equeue for slow->fast control signal queue");
    abort();
  }
  slow_fast_sig_qid = protoq->id;
  ret = cham_enable_queue(p, protoq->id, 0);
  if (ret != 0)
  {
    LOG_ERROR("failed to enable slow->fast control signal queue");
    abort();
  }

  protoq = cham_new_queue(p, ctx->config.ctrlq_len,
      sizeof(struct tcp_queue_bump_entry));
  if (protoq == NULL)
  {
    LOG_ERROR("failed to create slow->fast control packet queue");
    abort();
  }
  ctx->slow_fast_pkt_q = equeue_new(protoq->nelems, protoq->elsize,
      p->shm_base + protoq->off, protoq->off);
  if (ctx->slow_fast_pkt_q == NULL)
  {
    LOG_ERROR("failed to create equeue for slow->fast control packet queue");
    abort();
  }
  slow_fast_pkt_qid = protoq->id;

  ports = p->shm_base + port_map->off;
  for (i = 0; i <= MAX_PORT; i++)
  {
    ports[i].nsocks = 0;
    ports[i].next_sock = 0;
    for (j = 0; j < MAX_REUSOCK_PORT; j++)
      ports[i].sids[j] = ID_INVALID;
  }

  flows = p->shm_base + flow_map->off;
  for (i = 0; i < TCP_FLOW_BUCKETS; i++)
  {
    for (j = 0; j < TCP_FLOW_BUCKET_SLOTS; j++)
      flows[i].sids[j] = ID_INVALID;
  }

  cfg = p->shm_base + ctrl_map->off;
  cfg->fast_slow_sig_qid = fast_slow_sig_qid;
  cfg->fast_slow_pkt_qid = fast_slow_pkt_qid;
  cfg->slow_fast_sig_qid = slow_fast_sig_qid;
  cfg->slow_fast_pkt_qid = slow_fast_pkt_qid;

  listeners = calloc(MAX_SOCKETS, sizeof(*listeners));
  if (listeners == NULL)
  {
    LOG_ERROR("failed to allocate listener state");
    abort();
  }

  sock_meta = calloc(MAX_SOCKETS, sizeof(*sock_meta));
  if (sock_meta == NULL)
  {
    LOG_ERROR("failed to allocate socket metadata");
    abort();
  }
  for (i = 0; i < MAX_SOCKETS; i++)
    sock_meta[i].listener_id = ID_INVALID;

  bound_ports = calloc(MAX_PORT + 1, sizeof(*bound_ports));
  if (bound_ports == NULL)
  {
    LOG_ERROR("failed to allocate bound port state");
    abort();
  }
  for (i = 0; i <= MAX_PORT; i++)
  {
    for (j = 0; j < MAX_REUSOCK_PORT; j++)
      bound_ports[i].sids[j] = ID_INVALID;
  }

  ctx->app_uxfd = -1;
  ctx->app_epfd = -1;
  ctx->proto = p;
  ctx->n_apps = 0;
  ctx->next_app = 0;
  ctx->n_socks = 0;
  ctx->tomgr = tomgr;
  ctx->socks_map = socks_map;
  ctx->port_map = port_map;
  ctx->flow_map = flow_map;
  ctx->ctrl_map = ctrl_map;
  ctx->listeners = listeners;
  ctx->sock_meta = sock_meta;
  ctx->bound_ports = bound_ports;

  LOG_INFO("TCP initialized");
  return 0;
}

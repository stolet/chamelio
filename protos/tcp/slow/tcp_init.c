#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <cham_lib.h>
#include <unistd.h>

#include "tcp_internal.h"
#include "clock.h"
#include "queue_fns.h"
#include "log.h"
#include "tcp_config.h"
#include "tomgr.h"

/*** Init Helpers *************************************************************/

static struct proto_lib *tcp_proto_open(const struct tcp_slow_context *ctx);
static void tcp_ebpf_upload(struct proto_lib *proto);
static struct proto_map_lib *tcp_map_new(struct proto_lib *proto, __u32 nr,
    __u32 elsize, const char *what);
static struct proto_queue_lib *tcp_queue_new(struct proto_lib *proto, __u32 len,
    __u32 elsize, const char *what);
static struct dqueue *tcp_dqueue_open(struct proto_lib *proto,
    struct proto_queue_lib *protoq, const char *what);
static struct equeue *tcp_equeue_open(struct proto_lib *proto,
    struct proto_queue_lib *protoq, const char *what);
static void tcp_port_tbl_init(struct tcp_port *ports);
static void tcp_flow_tbl_init(struct tcp_flow_bucket *flows);
static struct tcp_listener_slow *tcp_listeners_alloc(void);
static struct tcp_sock_meta_slow *tcp_sock_meta_alloc(void);
static struct tcp_port *tcp_bound_tbl_alloc(void);

/*** Public API ***************************************************************/

int tcp_init(struct tcp_slow_context *ctx)
{
  int i;
  __u16 fast_slow_sig_qid;
  __u16 fast_slow_pkt_qid;
  __u16 slow_fast_sig_qid;
  __u16 slow_fast_pkt_qid;
  struct proto_lib *proto;
  struct proto_map_lib *ctrl_map;
  struct proto_map_lib *flow_map;
  struct proto_map_lib *port_map;
  struct proto_map_lib *socks_map;
  struct proto_queue_lib *protoq;
  struct tcp_ctrl_cfg *cfg;
  struct tcp_flow_bucket *flows;
  struct tcp_listener_slow *listeners;
  struct tcp_port *bound_ports;
  struct tcp_port *ports;
  struct tcp_sock_meta_slow *sock_meta;
  struct tomgr *tomgr;

  tomgr = tomgr_init();
  if (tomgr == NULL)
  {
    LOG_ERROR("failed to allocate timeout manager");
    abort();
  }
  if (clock_calibrate_tsc() != 0)
  {
    LOG_ERROR("failed to calibrate tsc");
    abort();
  }

  proto = tcp_proto_open(ctx);
  tcp_ebpf_upload(proto);

  port_map = tcp_map_new(proto, MAX_PORT + 1, sizeof(struct tcp_port),
      "listener port lookup");
  socks_map = tcp_map_new(proto, MAX_SOCKETS, sizeof(struct tcp_sock),
      "socket map");
  flow_map = tcp_map_new(proto, TCP_FLOW_BUCKETS,
      sizeof(struct tcp_flow_bucket), "TCP flow lookup");
  ctrl_map = tcp_map_new(proto, 1, sizeof(struct tcp_ctrl_cfg),
      "TCP control configuration");

  protoq = tcp_queue_new(proto, ctx->config.ctrlq_len,
      sizeof(struct tcp_queue_bump_entry), "fast->slow control signal queue");
  ctx->fast_slow_sig_q = tcp_dqueue_open(proto, protoq,
      "fast->slow control signal queue");
  fast_slow_sig_qid = protoq->id;

  protoq = tcp_queue_new(proto, ctx->config.ctrlq_len,
      sizeof(struct tcp_queue_bump_entry), "fast->slow control packet queue");
  ctx->fast_slow_pkt_q = tcp_dqueue_open(proto, protoq,
      "fast->slow control packet queue");
  fast_slow_pkt_qid = protoq->id;

  protoq = tcp_queue_new(proto, ctx->config.ctrlq_len,
      sizeof(struct tcp_queue_bump_entry), "slow->fast control signal queue");
  ctx->slow_fast_sig_q = tcp_equeue_open(proto, protoq,
      "slow->fast control signal queue");
  slow_fast_sig_qid = protoq->id;
  if (cham_enable_queue(proto, protoq->id, 0) != 0)
  {
    LOG_ERROR("failed to enable slow->fast control signal queue");
    abort();
  }

  protoq = tcp_queue_new(proto, ctx->config.ctrlq_len,
      sizeof(struct tcp_queue_bump_entry), "slow->fast control packet queue");
  ctx->slow_fast_pkt_q = tcp_equeue_open(proto, protoq,
      "slow->fast control packet queue");
  slow_fast_pkt_qid = protoq->id;

  ports = proto->shm_base + port_map->off;
  tcp_port_tbl_init(ports);

  flows = proto->shm_base + flow_map->off;
  tcp_flow_tbl_init(flows);

  cfg = proto->shm_base + ctrl_map->off;
  cfg->fast_slow_sig_qid = fast_slow_sig_qid;
  cfg->fast_slow_pkt_qid = fast_slow_pkt_qid;
  cfg->slow_fast_sig_qid = slow_fast_sig_qid;
  cfg->slow_fast_pkt_qid = slow_fast_pkt_qid;

  listeners = tcp_listeners_alloc();
  sock_meta = tcp_sock_meta_alloc();
  bound_ports = tcp_bound_tbl_alloc();

  ctx->app_uxfd = -1;
  ctx->app_epfd = -1;
  ctx->proto = proto;
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
  ctx->cc_poll_tsc = clock_rdtsc();
  ctx->cc_next_sock = 0;
  for (i = 0; i < MAX_SOCKETS; i++)
    ctx->sock_meta[i].listener_id = ID_INVALID;

  LOG_INFO("TCP initialized");
  return 0;
}

/*** Init Helpers *************************************************************/

static struct proto_lib *tcp_proto_open(const struct tcp_slow_context *ctx)
{
  struct guest_lib *guest;
  struct proto_lib *proto;

  if (!ctx->config.virt)
  {
    guest = cham_connect_guest();
    if (guest == NULL)
    {
      LOG_ERROR("TCP slow-path couldn't connect to Chamelio");
      abort();
    }

    proto = cham_new_proto_bare(guest);
  }
  else
  {
    proto = cham_new_proto_virt();
  }

  if (proto == NULL)
  {
    LOG_ERROR("TCP slow-path failed to register protocol with Chamelio");
    abort();
  }

  return proto;
}

static void tcp_ebpf_upload(struct proto_lib *proto)
{
  int fd;
  struct stat statbuf;
  struct proto_ebpf_lib *ebpf;
  __u8 *bytecode;

  fd = open(TCP_EBPF_BYTECODE, O_RDWR);
  if (fd < 0)
  {
    LOG_ERROR("Failed to open TCP eBPF bytecode");
    abort();
  }
  fstat(fd, &statbuf);

  ebpf = cham_allocate_ebpf(proto, statbuf.st_size);
  if (ebpf == NULL)
  {
    LOG_ERROR("Failed to allocate space for eBPF bytecode in shared memory");
    abort();
  }

  bytecode = mmap(NULL, ebpf->size, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
  if (bytecode == MAP_FAILED)
  {
    LOG_ERROR("failed to mmap eBPF bytecode");
    abort();
  }
  if (cham_upload_ebpf(proto, bytecode, ebpf->size) != 0)
  {
    LOG_ERROR("failed to upload eBPF bytecode to shared memory");
    abort();
  }
}

static struct proto_map_lib *tcp_map_new(struct proto_lib *proto, __u32 nr,
    __u32 elsize, const char *what)
{
  struct proto_map_lib *map;

  map = cham_new_map(proto, nr, elsize);
  if (map == NULL)
  {
    LOG_ERROR("failed to create map for %s", what);
    abort();
  }

  return map;
}

static struct proto_queue_lib *tcp_queue_new(struct proto_lib *proto, __u32 len,
    __u32 elsize, const char *what)
{
  struct proto_queue_lib *protoq;

  protoq = cham_new_queue(proto, len, elsize);
  if (protoq == NULL)
  {
    LOG_ERROR("failed to create %s", what);
    abort();
  }

  return protoq;
}

static struct dqueue *tcp_dqueue_open(struct proto_lib *proto,
    struct proto_queue_lib *protoq, const char *what)
{
  struct dqueue *q;

  q = dqueue_new(protoq->nelems, protoq->elsize, proto->shm_base + protoq->off,
      protoq->off);
  if (q == NULL)
  {
    LOG_ERROR("failed to create dqueue for %s", what);
    abort();
  }

  return q;
}

static struct equeue *tcp_equeue_open(struct proto_lib *proto,
    struct proto_queue_lib *protoq, const char *what)
{
  struct equeue *q;

  q = equeue_new(protoq->nelems, protoq->elsize, proto->shm_base + protoq->off,
      protoq->off);
  if (q == NULL)
  {
    LOG_ERROR("failed to create equeue for %s", what);
    abort();
  }

  return q;
}

static void tcp_port_tbl_init(struct tcp_port *ports)
{
  int i;
  int j;

  for (i = 0; i <= MAX_PORT; i++)
  {
    ports[i].nsocks = 0;
    ports[i].next_sock = 0;
    for (j = 0; j < MAX_REUSOCK_PORT; j++)
      ports[i].sids[j] = ID_INVALID;
  }
}

static void tcp_flow_tbl_init(struct tcp_flow_bucket *flows)
{
  int i;
  int j;

  for (i = 0; i < TCP_FLOW_BUCKETS; i++)
  {
    for (j = 0; j < TCP_FLOW_BUCKET_SLOTS; j++)
      flows[i].sids[j] = ID_INVALID;
  }
}

static struct tcp_listener_slow *tcp_listeners_alloc(void)
{
  struct tcp_listener_slow *listeners;

  listeners = calloc(MAX_SOCKETS, sizeof(*listeners));
  if (listeners == NULL)
  {
    LOG_ERROR("failed to allocate listener state");
    abort();
  }

  return listeners;
}

static struct tcp_sock_meta_slow *tcp_sock_meta_alloc(void)
{
  struct tcp_sock_meta_slow *sock_meta;

  sock_meta = calloc(MAX_SOCKETS, sizeof(*sock_meta));
  if (sock_meta == NULL)
  {
    LOG_ERROR("failed to allocate socket metadata");
    abort();
  }

  return sock_meta;
}

static struct tcp_port *tcp_bound_tbl_alloc(void)
{
  int i;
  int j;
  struct tcp_port *ports;

  ports = calloc(MAX_PORT + 1, sizeof(*ports));
  if (ports == NULL)
  {
    LOG_ERROR("failed to allocate bound port state");
    abort();
  }

  for (i = 0; i <= MAX_PORT; i++)
  {
    for (j = 0; j < MAX_REUSOCK_PORT; j++)
      ports[i].sids[j] = ID_INVALID;
  }

  return ports;
}

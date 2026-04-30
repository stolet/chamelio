#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "udp_slow.h"
#include "udp_state.h"

static int state_read(const char *path, struct udp_state *state);
static int proc_read(pid_t pid, __u64 addr, void *buf, size_t len,
    const char *what);
static const char *ip_str(__u32 ip, char *buf, size_t len);
static void sock_print(const struct udp_sock *sock);
static void port_print(const struct udp_port *ports, __u32 nr);
static void apps_print(pid_t pid, const struct udp_slow_context *ctx,
    __u32 n_fp_cores);
static void dq_print(pid_t pid, const char *name, const struct dqueue *ptr);
static void eq_print(pid_t pid, const char *name, const struct equeue *ptr);
static void protoq_print(pid_t pid, const char *name,
    const struct proto_queue_lib *ptr);

int main(int argc, char **argv)
{
  pid_t pid;
  const char *path;
  struct udp_state state;
  struct udp_slow_context ctx;
  struct proto_lib proto;
  struct proto_map_lib socks_map;
  struct proto_map_lib port_map;
  struct proto_map_lib cfg_map;
  struct udp_sock *socks;
  struct udp_port *ports;
  struct udp_cfg cfg;
  __u32 i;

  path = argc > 1 ? argv[1] : UDP_STATE_PATH;
  if (state_read(path, &state) != 0)
    return 1;

  if (state.magic != UDP_STATE_MAGIC || state.version != UDP_STATE_VERSION)
  {
    fprintf(stderr, "bad UDP state in %s\n", path);
    return 1;
  }
  if (state.ctx_size != sizeof(ctx) ||
      state.sock_size != sizeof(struct udp_sock) ||
      state.port_size != sizeof(struct udp_port) ||
      state.cfg_size != sizeof(struct udp_cfg) ||
      state.app_size != sizeof(struct udp_app_slow) ||
      state.app_ctx_size != sizeof(struct udp_app_context_slow))
  {
    fprintf(stderr, "UDP state ABI mismatch, rebuild udp_slow and statetool\n");
    return 1;
  }

  pid = state.pid;
  if (proc_read(pid, state.ctx_addr, &ctx, sizeof(ctx), "udp_slow_context") != 0)
    return 1;
  if (proc_read(pid, (__u64) ctx.proto, &proto, sizeof(proto), "proto_lib") != 0)
    return 1;
  if (proc_read(pid, (__u64) ctx.socks_map, &socks_map, sizeof(socks_map),
      "socks_map") != 0)
    return 1;
  if (proc_read(pid, (__u64) ctx.port_map, &port_map, sizeof(port_map),
      "port_map") != 0)
    return 1;
  if (proc_read(pid, (__u64) ctx.cfg_map, &cfg_map, sizeof(cfg_map),
      "cfg_map") != 0)
    return 1;
  if (ctx.n_socks > socks_map.nelems || ctx.n_apps > MAX_APPS ||
      proto.n_fp_cores > MAX_FP_CORES ||
      socks_map.nelems != MAX_SOCKETS ||
      socks_map.elsize != sizeof(struct udp_sock) ||
      port_map.nelems != MAX_PORT ||
      port_map.elsize != sizeof(struct udp_port) ||
      cfg_map.nelems != 1 || cfg_map.elsize != sizeof(struct udp_cfg))
  {
    fprintf(stderr, "unexpected UDP table dimensions\n");
    return 1;
  }

  socks = calloc(socks_map.nelems, sizeof(*socks));
  ports = calloc(port_map.nelems, sizeof(*ports));
  if (socks == NULL || ports == NULL)
  {
    fprintf(stderr, "allocation failed\n");
    return 1;
  }

  if (proc_read(pid, (__u64) proto.shm_base + socks_map.off, socks,
      socks_map.nelems * sizeof(*socks), "socket map") != 0)
    return 1;
  if (proc_read(pid, (__u64) proto.shm_base + port_map.off, ports,
      port_map.nelems * sizeof(*ports), "port map") != 0)
    return 1;
  if (proc_read(pid, (__u64) proto.shm_base + cfg_map.off, &cfg, sizeof(cfg),
      "UDP config") != 0)
    return 1;

  printf("udp_slow pid=%u ctx=0x%" PRIx64 " n_socks=%u n_apps=%u\n",
      state.pid, (uint64_t) state.ctx_addr, ctx.n_socks, ctx.n_apps);
  printf("proto shm_base=%p shm_size=%u local_ip=%s n_fp_cores=%u\n",
      proto.shm_base, proto.shm_size,
      ip_str(proto.local_ip, (char[INET_ADDRSTRLEN]){0}, INET_ADDRSTRLEN),
      proto.n_fp_cores);
  printf("maps socks id=%u nr=%u elsize=%u off=%" PRIu64
      " ports id=%u nr=%u elsize=%u off=%" PRIu64
      " cfg id=%u off=%" PRIu64 "\n",
      socks_map.id, socks_map.nelems, socks_map.elsize,
      (uint64_t) socks_map.off,
      port_map.id, port_map.nelems, port_map.elsize,
      (uint64_t) port_map.off,
      cfg_map.id, (uint64_t) cfg_map.off);
  printf("cfg next_port=%u\n", cfg.next_port);

  printf("\nsocket table\n");
  for (i = 0; i < ctx.n_socks; i++)
    sock_print(&socks[i]);

  printf("\nports\n");
  port_print(ports, port_map.nelems);

  printf("\napps\n");
  apps_print(pid, &ctx, proto.n_fp_cores);

  return 0;
}

static int state_read(const char *path, struct udp_state *state)
{
  FILE *f;

  f = fopen(path, "rb");
  if (f == NULL)
  {
    fprintf(stderr, "failed to open %s: %s\n", path, strerror(errno));
    return -1;
  }
  if (fread(state, sizeof(*state), 1, f) != 1)
  {
    fprintf(stderr, "failed to read %s\n", path);
    fclose(f);
    return -1;
  }
  fclose(f);
  return 0;
}

static int proc_read(pid_t pid, __u64 addr, void *buf, size_t len,
    const char *what)
{
  ssize_t ret;
  struct iovec local = {
    .iov_base = buf,
    .iov_len = len,
  };
  struct iovec remote = {
    .iov_base = (void *) (uintptr_t) addr,
    .iov_len = len,
  };

  ret = process_vm_readv(pid, &local, 1, &remote, 1, 0);
  if (ret != (ssize_t) len)
  {
    fprintf(stderr, "failed to read %s at 0x%" PRIx64 ": %s\n", what,
        (uint64_t) addr, ret < 0 ? strerror(errno) : "short read");
    return -1;
  }

  return 0;
}

static const char *ip_str(__u32 ip, char *buf, size_t len)
{
  struct in_addr in = {
    .s_addr = ip,
  };

  if (inet_ntop(AF_INET, &in, buf, len) == NULL)
    snprintf(buf, len, "0x%08x", ip);
  return buf;
}

static void sock_print(const struct udp_sock *sock)
{
  char lip[INET_ADDRSTRLEN];

  printf("sock[%u] opaque=0x%" PRIx64 " core=%u app_bump_qid=%u "
      "reuseport=%u local=%s:%u\n",
      sock->id, (uint64_t) sock->opaque, sock->core, sock->app_bump_qid,
      sock->reuport, ip_str(sock->local_ip, lip, sizeof(lip)),
      sock->local_port);
  printf("  rx len=%u avail=%u head=%u off=%" PRIu64
      " tx len=%u avail=%u head=%u off=%" PRIu64 "\n",
      sock->rx_len, sock->rx_avail, sock->rx_head,
      (uint64_t) sock->rx_off, sock->tx_len, sock->tx_avail, sock->tx_head,
      (uint64_t) sock->tx_off);
}

static void port_print(const struct udp_port *ports, __u32 nr)
{
  __u32 i;
  __u32 j;

  for (i = 0; i < nr; i++)
  {
    if (ports[i].nsocks == 0)
      continue;
    printf("port[%u] nsocks=%u next_sock=%u sids=", i, ports[i].nsocks,
        ports[i].next_sock);
    for (j = 0; j < MAX_REUSOCK_PORT; j++)
      printf("%s%u", j == 0 ? "" : ",", ports[i].sids[j]);
    printf("\n");
  }
}

static void apps_print(pid_t pid, const struct udp_slow_context *ctx,
    __u32 n_fp_cores)
{
  const struct udp_app_slow *app;
  const struct udp_app_context_slow *actx;
  __u32 app_id;
  __u32 ctx_id;
  __u32 core;

  for (app_id = 0; app_id < ctx->n_apps; app_id++)
  {
    app = &ctx->apps[app_id];
    if (app->n_ctxs > MAX_CTXS)
    {
      printf("app[%u] invalid n_ctxs=%u\n", app_id, app->n_ctxs);
      continue;
    }
    printf("app[%u] id=%u n_ctxs=%u next_ctx=%u\n", app_id, app->id,
        app->n_ctxs, app->next_ctx);
    for (ctx_id = 0; ctx_id < app->n_ctxs; ctx_id++)
    {
      actx = &app->ctxs[ctx_id];
      printf("  ctx[%u] id=%u app=%p\n", ctx_id, actx->id, actx->app);
      dq_print(pid, "app_slow_q", actx->app_slow_q);
      eq_print(pid, "slow_app_q", actx->slow_app_q);
      if (n_fp_cores > MAX_FP_CORES)
        n_fp_cores = MAX_FP_CORES;
      for (core = 0; core < n_fp_cores; core++)
      {
        protoq_print(pid, "app_bump_q", actx->app_bump_qs[core]);
        protoq_print(pid, "fast_bump_q", actx->fast_bump_qs[core]);
      }
    }
  }
}

static void dq_print(pid_t pid, const char *name, const struct dqueue *ptr)
{
  struct dqueue q;

  if (ptr == NULL)
  {
    printf("    %s=NULL\n", name);
    return;
  }
  if (proc_read(pid, (__u64) ptr, &q, sizeof(q), name) != 0)
    return;
  printf("    %s=%p head=%u nr=%u elsize=%zu off=%" PRIu64 "\n",
      name, ptr, q.head, q.nelems, q.elsize, (uint64_t) q.off);
}

static void eq_print(pid_t pid, const char *name, const struct equeue *ptr)
{
  struct equeue q;

  if (ptr == NULL)
  {
    printf("    %s=NULL\n", name);
    return;
  }
  if (proc_read(pid, (__u64) ptr, &q, sizeof(q), name) != 0)
    return;
  printf("    %s=%p tail=%u nr=%u elsize=%zu off=%" PRIu64 "\n",
      name, ptr, q.tail, q.nelems, q.elsize, (uint64_t) q.off);
}

static void protoq_print(pid_t pid, const char *name,
    const struct proto_queue_lib *ptr)
{
  struct proto_queue_lib q;

  if (ptr == NULL)
    return;
  if (proc_read(pid, (__u64) ptr, &q, sizeof(q), name) != 0)
    return;
  printf("    %s id=%u nr=%u elsize=%u off=%" PRIu64 "\n",
      name, q.id, q.nelems, q.elsize, (uint64_t) q.off);
}

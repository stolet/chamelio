#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "tcp_slow.h"
#include "tcp_state.h"

static int state_read(const char *path, struct tcp_state *state);
static int proc_read(pid_t pid, __u64 addr,
    void *buf, size_t len, const char *what);
static const char *ip_str(__u32 ip, char *buf, size_t len);
static const char *state_name(__u8 state);
static void sock_print(const struct tcp_sock *sock);
static void port_print(const char *name,
    const struct tcp_port *ports, __u32 nr);
static void flow_print(const struct tcp_flow_bucket *flows);
static void meta_print(const struct tcp_sock_meta_slow *meta, __u32 sid);
static void listener_print(pid_t pid,
    const struct tcp_listener_slow *listener, __u32 sid);

int main(int argc, char **argv)
{
  pid_t pid;
  const char *path;
  struct tcp_state state;
  struct tcp_slow_context ctx;
  struct proto_lib proto;
  struct proto_map_lib socks_map;
  struct proto_map_lib port_map;
  struct proto_map_lib flow_map;
  struct proto_map_lib ctl_map;
  struct tcp_ctl_cfg cfg;
  struct tcp_sock *socks;
  struct tcp_port *ports;
  struct tcp_port *bound_ports;
  struct tcp_flow_bucket *flows;
  struct tcp_listener_slow *listeners;
  struct tcp_sock_meta_slow *meta;
  __u32 i;

  path = argc > 1 ? argv[1] : TCP_STATE_PATH;
  if (state_read(path, &state) != 0)
    return 1;

  if (state.magic != TCP_STATE_MAGIC || state.version != TCP_STATE_VERSION)
  {
    fprintf(stderr, "bad TCP state in %s\n", path);
    return 1;
  }
  if (state.ctx_size != sizeof(ctx) ||
      state.sock_size != sizeof(struct tcp_sock) ||
      state.port_size != sizeof(struct tcp_port) ||
      state.flow_bucket_size != sizeof(struct tcp_flow_bucket) ||
      state.ctl_cfg_size != sizeof(struct tcp_ctl_cfg) ||
      state.listener_size != sizeof(struct tcp_listener_slow) ||
      state.meta_size != sizeof(struct tcp_sock_meta_slow))
  {
    fprintf(stderr, "TCP state ABI mismatch, rebuild tcp_slow and statetool\n");
    return 1;
  }

  pid = state.pid;
  if (proc_read(pid, state.ctx_addr, &ctx, sizeof(ctx), "tcp_slow_context") != 0)
    return 1;
  if (proc_read(pid, (__u64) ctx.proto, &proto, sizeof(proto), "proto_lib") != 0)
    return 1;
  if (proc_read(pid, (__u64) ctx.socks_map, &socks_map, sizeof(socks_map),
      "socks_map") != 0)
    return 1;
  if (proc_read(pid, (__u64) ctx.port_map, &port_map, sizeof(port_map),
      "port_map") != 0)
    return 1;
  if (proc_read(pid, (__u64) ctx.flow_map, &flow_map, sizeof(flow_map),
      "flow_map") != 0)
    return 1;
  if (proc_read(pid, (__u64) ctx.ctl_map, &ctl_map, sizeof(ctl_map),
      "ctl_map") != 0)
    return 1;
  if (ctx.n_socks > MAX_SOCKETS || socks_map.nelems != MAX_SOCKETS ||
      port_map.nelems != MAX_PORT + 1 || flow_map.nelems != TCP_FLOW_BUCKETS)
  {
    fprintf(stderr, "unexpected TCP table dimensions\n");
    return 1;
  }

  socks = calloc(socks_map.nelems, sizeof(*socks));
  ports = calloc(port_map.nelems, sizeof(*ports));
  bound_ports = calloc(MAX_PORT + 1, sizeof(*bound_ports));
  flows = calloc(flow_map.nelems, sizeof(*flows));
  listeners = calloc(ctx.n_socks ? ctx.n_socks : 1, sizeof(*listeners));
  meta = calloc(ctx.n_socks ? ctx.n_socks : 1, sizeof(*meta));
  if (socks == NULL || ports == NULL || bound_ports == NULL || flows == NULL ||
      listeners == NULL || meta == NULL)
  {
    fprintf(stderr, "allocation failed\n");
    return 1;
  }

  if (proc_read(pid, (__u64) proto.shm_base + socks_map.off, socks,
      socks_map.nelems * sizeof(*socks), "socket map") != 0)
    return 1;
  if (proc_read(pid, (__u64) proto.shm_base + port_map.off, ports,
      port_map.nelems * sizeof(*ports), "listener port map") != 0)
    return 1;
  if (proc_read(pid, (__u64) proto.shm_base + flow_map.off, flows,
      flow_map.nelems * sizeof(*flows), "flow map") != 0)
    return 1;
  if (proc_read(pid, (__u64) proto.shm_base + ctl_map.off, &cfg, sizeof(cfg),
      "control config") != 0)
    return 1;
  if (ctx.n_socks != 0 &&
      proc_read(pid, (__u64) ctx.listeners, listeners,
      ctx.n_socks * sizeof(*listeners), "listeners") != 0)
    return 1;
  if (ctx.n_socks != 0 &&
      proc_read(pid, (__u64) ctx.sock_meta, meta, ctx.n_socks * sizeof(*meta),
      "socket metadata") != 0)
    return 1;
  if (proc_read(pid, (__u64) ctx.bound_ports, bound_ports,
      (MAX_PORT + 1) * sizeof(*bound_ports), "bound ports") != 0)
    return 1;

  printf("tcp_slow pid=%u ctx=0x%" PRIx64 " n_socks=%u n_apps=%u\n",
      state.pid, (uint64_t) state.ctx_addr, ctx.n_socks, ctx.n_apps);
  printf("proto shm_base=%p shm_size=%u local_ip=%s n_fp_cores=%u\n",
      proto.shm_base, proto.shm_size,
      ip_str(proto.local_ip, (char[INET_ADDRSTRLEN]){0}, INET_ADDRSTRLEN),
      proto.n_fp_cores);
  printf("maps socks id=%u nr=%u elsize=%u off=%" PRIu64
      " ports id=%u nr=%u off=%" PRIu64 " flows id=%u nr=%u off=%" PRIu64
      " ctl id=%u off=%" PRIu64 "\n",
      socks_map.id, socks_map.nelems, socks_map.elsize,
      (uint64_t) socks_map.off,
      port_map.id, port_map.nelems, (uint64_t) port_map.off,
      flow_map.id, flow_map.nelems, (uint64_t) flow_map.off,
      ctl_map.id, (uint64_t) ctl_map.off);
  printf("ctl_cfg fast_slow_sig_qid[0]=%u fast_slow_pkt_qid[0]=%u "
      "slow_fast_sig_qid=%u slow_fast_pkt_qid=%u\n",
      cfg.fast_slow_sig_qids[0], cfg.fast_slow_pkt_qids[0],
      cfg.slow_fast_sig_qid, cfg.slow_fast_pkt_qid);

  printf("\nsocket table\n");
  for (i = 0; i < ctx.n_socks; i++)
  {
    sock_print(&socks[i]);
    meta_print(&meta[i], i);
    listener_print(pid, &listeners[i], i);
  }

  printf("\nlistener ports\n");
  port_print("listen", ports, port_map.nelems);
  printf("\nbound ports\n");
  port_print("bound", bound_ports, MAX_PORT + 1);
  printf("\nflow buckets\n");
  flow_print(flows);

  return 0;
}

static int state_read(const char *path, struct tcp_state *state)
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

static const char *state_name(__u8 state)
{
  switch (state)
  {
    case TCP_SOCK_STATE_CLOSED:
      return "CLOSED";
    case TCP_SOCK_STATE_INIT:
      return "INIT";
    case TCP_SOCK_STATE_LISTEN:
      return "LISTEN";
    case TCP_SOCK_STATE_SYN_SENT:
      return "SYN_SENT";
    case TCP_SOCK_STATE_SYN_RECV:
      return "SYN_RECV";
    case TCP_SOCK_STATE_ACCEPT_PENDING:
      return "ACCEPT_PENDING";
    case TCP_SOCK_STATE_ESTABLISHED:
      return "ESTABLISHED";
    case TCP_SOCK_STATE_FIN_WAIT1:
      return "FIN_WAIT1";
    default:
      return "UNKNOWN";
  }
}

static void sock_print(const struct tcp_sock *sock)
{
  char lip[INET_ADDRSTRLEN];
  char rip[INET_ADDRSTRLEN];

  printf("sock[%u] state=%s(%u) opaque=0x%" PRIx64 " core=%u app_bump_qid=%u "
      "app_id=%u ctx_id=%u reuport=%u flags=0x%x lock=%u\n",
      sock->id, state_name(sock->state), sock->state,
      (uint64_t) sock->opaque, sock->core, sock->app_bump_qid, sock->app_id,
      sock->ctx_id, sock->reuport, sock->flags, sock->lock);
  printf("  local=%s:%u remote=%s:%u\n",
      ip_str(sock->local_ip, lip, sizeof(lip)), sock->local_port,
      ip_str(sock->remote_ip, rip, sizeof(rip)), sock->remote_port);
  printf("  rx len=%u avail=%u head=%u off=%" PRIu64
      " tx len=%u avail=%u head=%u off=%" PRIu64 "\n",
      sock->rx_len, sock->rx_avail, sock->rx_head,
      (uint64_t) sock->rx_off, sock->tx_len, sock->tx_avail, sock->tx_head,
      (uint64_t) sock->tx_off);
  printf("  seq tx_seq=%u tx_pending=%u rx_seq=%u tx_remote_avail=%u "
      "tx_rexmit_seq=%u tx_rexmit_end_seq=%u\n",
      sock->tx_seq, sock->tx_pending, sock->rx_seq, sock->tx_remote_avail,
      sock->tx_rexmit_seq, sock->tx_rexmit_end_seq);
  printf("  cc rx_dupack_cnt=%u cc_acks=%u cc_ackb=%u cc_ecnb=%u cc_drops=%u "
      "cc_rate=%u ts_recent=%u rtt_est=%u\n",
      sock->rx_dupack_cnt, sock->cc_acks, sock->cc_ackb, sock->cc_ecnb,
      sock->cc_drops, sock->cc_rate, sock->ts_recent, sock->rtt_est);
  printf("  pacing tx_ready_tsc=%" PRIu64 " recovery_active=%u "
      "recovery_end_seq=%u\n",
      (uint64_t) sock->tx_ready_tsc, sock->recovery_active,
      sock->recovery_end_seq);
}

static void port_print(const char *name, const struct tcp_port *ports,
    __u32 nr)
{
  __u32 i;
  __u32 j;

  for (i = 0; i < nr; i++)
  {
    if (ports[i].nsocks == 0)
      continue;
    printf("%s_port[%u] nsocks=%u next_sock=%u sids=", name, i,
        ports[i].nsocks, ports[i].next_sock);
    for (j = 0; j < MAX_REUSOCK_PORT; j++)
      printf("%s%u", j == 0 ? "" : ",", ports[i].sids[j]);
    printf("\n");
  }
}

static void flow_print(const struct tcp_flow_bucket *flows)
{
  __u32 i;
  __u32 j;
  int used;

  for (i = 0; i < TCP_FLOW_BUCKETS; i++)
  {
    used = 0;
    for (j = 0; j < TCP_FLOW_BUCKET_SLOTS; j++)
      used |= flows[i].sids[j] != ID_INVALID;
    if (!used)
      continue;
    printf("flow[%u] sids=", i);
    for (j = 0; j < TCP_FLOW_BUCKET_SLOTS; j++)
      printf("%s%u", j == 0 ? "" : ",", flows[i].sids[j]);
    printf("\n");
  }
}

static void meta_print(const struct tcp_sock_meta_slow *meta, __u32 sid)
{
  printf("  meta[%u] listener_id=%u auto_bound=%u timer=%p retx_kind=%u "
      "retx_left=%u cc_tsc=%" PRIu64 " cc_rtt=%u\n",
      sid, meta->listener_id, meta->auto_bound, meta->timer, meta->retx_kind,
      meta->retx_left, (uint64_t) meta->cc_tsc, meta->cc_rtt);
  printf("  meta_cc last_drops=%u last_acks=%u last_ackb=%u last_ecnb=%u "
      "rexmits=%u cnt_tx_pending=%u ts_tx_pending=%" PRIu64 "\n",
      meta->cc_last_drops, meta->cc_last_acks, meta->cc_last_ackb,
      meta->cc_last_ecnb, meta->cc_rexmits, meta->cnt_tx_pending,
      (uint64_t) meta->ts_tx_pending);
  printf("  dctcp unproc_acks=%u unproc_ackb=%u unproc_ecnb=%u "
      "unproc_drops=%u ecn_rate=%u act_rate=%u slowstart=%u\n",
      meta->dctcp_unproc_acks, meta->dctcp_unproc_ackb,
      meta->dctcp_unproc_ecnb, meta->dctcp_unproc_drops,
      meta->dctcp_ecn_rate, meta->dctcp_act_rate, meta->dctcp_slowstart);
}

static void listener_print(pid_t pid, const struct tcp_listener_slow *listener,
    __u32 sid)
{
  __u32 i;
  __u32 *ready;

  printf("  listener[%u] active=%u sock_id=%u backlog_len=%u backlog_used=%u "
      "ready_head=%u ready_used=%u ready_sids=%p\n",
      sid, listener->active, listener->sock_id, listener->backlog_len,
      listener->backlog_used, listener->ready_head, listener->ready_used,
      listener->ready_sids);
  if (!listener->active || listener->ready_sids == NULL ||
      listener->backlog_len == 0)
    return;

  ready = calloc(listener->backlog_len, sizeof(*ready));
  if (ready == NULL)
    return;
  if (proc_read(pid, (__u64) listener->ready_sids, ready,
      listener->backlog_len * sizeof(*ready), "listener ready_sids") != 0)
  {
    free(ready);
    return;
  }

  printf("  listener_ready[%u] sids=", sid);
  for (i = 0; i < listener->backlog_len; i++)
    printf("%s%u", i == 0 ? "" : ",", ready[i]);
  printf("\n");
  free(ready);
}

#include "tcp_internal.h"
#include "log.h"

/*** Flow Helpers *************************************************************/

static inline __u32 flow_hash(__u32 local_ip, __u16 local_port, __u32 remote_ip,
    __u16 remote_port);

/*** Flow API *****************************************************************/

int tcp_flow_insert(struct tcp_slow_context *ctx, struct tcp_sock *sock)
{
  int i;
  __u32 hash;
  struct tcp_flow_bucket *bucket;

  hash = flow_hash(sock->local_ip, sock->local_port, sock->remote_ip,
      sock->remote_port) % TCP_FLOW_BUCKETS;
  bucket = &tcp_flow_map(ctx)[hash];

  for (i = 0; i < TCP_FLOW_BUCKET_SLOTS; i++)
  {
    if (bucket->sids[i] != ID_INVALID)
      continue;
    bucket->sids[i] = sock->id;
    return 0;
  }

  LOG_WARN("TCP flow bucket full for local_port=%u remote_port=%u",
      sock->local_port, sock->remote_port);
  return -1;
}

void tcp_flow_remove(struct tcp_slow_context *ctx, struct tcp_sock *sock)
{
  int i;
  __u32 hash;
  struct tcp_flow_bucket *bucket;

  hash = flow_hash(sock->local_ip, sock->local_port, sock->remote_ip,
      sock->remote_port) % TCP_FLOW_BUCKETS;
  bucket = &tcp_flow_map(ctx)[hash];

  for (i = 0; i < TCP_FLOW_BUCKET_SLOTS; i++)
  {
    if (bucket->sids[i] != sock->id)
      continue;
    bucket->sids[i] = ID_INVALID;
    return;
  }
}

struct tcp_sock *tcp_flow_lookup(struct tcp_slow_context *ctx, __u32 local_ip,
    __u16 local_port, __u32 remote_ip, __u16 remote_port)
{
  int i;
  __u32 hash;
  __u32 sid;
  struct tcp_flow_bucket *bucket;
  struct tcp_sock *socks;
  struct tcp_sock *sock;

  hash = flow_hash(local_ip, local_port, remote_ip, remote_port) %
      TCP_FLOW_BUCKETS;
  bucket = &tcp_flow_map(ctx)[hash];
  socks = tcp_sock_map(ctx);

  for (i = 0; i < TCP_FLOW_BUCKET_SLOTS; i++)
  {
    sid = bucket->sids[i];
    if (sid == ID_INVALID)
      continue;
    sock = &socks[sid];
    if (sock->state == TCP_SOCK_STATE_CLOSED)
      continue;
    if (sock->local_ip == local_ip && sock->local_port == local_port &&
        sock->remote_ip == remote_ip && sock->remote_port == remote_port)
    {
      return sock;
    }
  }

  return NULL;
}

/*** Flow Helpers *************************************************************/

static inline __u32 flow_hash(__u32 local_ip, __u16 local_port, __u32 remote_ip,
    __u16 remote_port)
{
  __u32 hash;

  hash = local_ip ^ remote_ip ^ ((__u32) local_port << 16) ^ remote_port;
  return hash ^ (hash >> 16);
}

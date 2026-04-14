#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "tcp_internal.h"
#include "log.h"

/*** Listener Helpers *********************************************************/

static void listener_backlog_put(struct tcp_listener_slow *listener);

/*** Listener API *************************************************************/

int tcp_listener_insert(struct tcp_slow_context *ctx, __u16 port, __u32 sock_id,
    int reuseport)
{
  int i;
  struct tcp_port *listeners;
  struct tcp_port *entry;
  struct tcp_sock *socks;

  listeners = tcp_listener_map(ctx);
  entry = &listeners[port];
  socks = tcp_sock_map(ctx);

  if (entry->nsocks == 0)
  {
    entry->sids[0] = sock_id;
    entry->nsocks = 1;
    entry->next_sock = 0;
    return 0;
  }

  if (!reuseport || entry->nsocks >= MAX_REUSOCK_PORT)
    return -EADDRINUSE;

  for (i = 0; i < entry->nsocks; i++)
  {
    if (!socks[entry->sids[i]].reuport)
      return -EADDRINUSE;
  }

  entry->sids[entry->nsocks] = sock_id;
  entry->nsocks++;
  return 0;
}

void tcp_listener_remove(struct tcp_slow_context *ctx, __u16 port, __u32 sock_id)
{
  int i;
  struct tcp_port *listeners;
  struct tcp_port *entry;

  if (port == 0 || port > MAX_PORT)
    return;

  listeners = tcp_listener_map(ctx);
  entry = &listeners[port];
  for (i = 0; i < entry->nsocks; i++)
  {
    if (entry->sids[i] != sock_id)
      continue;

    for (; i + 1 < entry->nsocks; i++)
      entry->sids[i] = entry->sids[i + 1];
    entry->nsocks--;
    entry->sids[entry->nsocks] = ID_INVALID;
    if (entry->next_sock >= entry->nsocks)
      entry->next_sock = 0;
    return;
  }
}

__u32 tcp_listener_lookup(struct tcp_slow_context *ctx, __u32 local_ip,
    __u16 port)
{
  struct tcp_port *listeners;
  struct tcp_port *entry;
  struct tcp_sock *socks;
  __u32 sid;
  int i;
  int idx;

  if (port == 0 || port > MAX_PORT)
    return ID_INVALID;

  listeners = tcp_listener_map(ctx);
  entry = &listeners[port];
  socks = tcp_sock_map(ctx);

  if (entry->nsocks == 0)
    return ID_INVALID;

  for (i = 0; i < entry->nsocks; i++)
  {
    idx = (entry->next_sock + i) % entry->nsocks;
    sid = entry->sids[idx];
    if (sid == ID_INVALID)
      continue;
    if (socks[sid].state != TCP_SOCK_STATE_LISTEN)
      continue;
    if (socks[sid].local_ip != 0 && socks[sid].local_ip != local_ip)
      continue;
    entry->next_sock = (idx + 1) % entry->nsocks;
    return sid;
  }

  return ID_INVALID;
}

void tcp_listener_reset(struct tcp_slow_context *ctx, __u32 listener_id)
{
  struct tcp_listener_slow *listener;

  listener = &ctx->listeners[listener_id];
  free(listener->ready_sids);
  memset(listener, 0, sizeof(*listener));
}

void tcp_listener_detach_child(struct tcp_slow_context *ctx, struct tcp_sock *sock)
{
  __u32 i;
  __u32 listener_id;
  struct tcp_listener_slow *listener;

  listener_id = tcp_sock_meta(ctx, sock)->listener_id;
  if (listener_id == ID_INVALID)
    return;

  listener = &ctx->listeners[listener_id];
  if (listener->active && listener->ready_sids != NULL)
  {
    for (i = 0; i < listener->backlog_len; i++)
    {
      if (listener->ready_sids[i] != sock->id)
        continue;

      listener->ready_sids[i] = ID_INVALID;
      break;
    }
  }

  listener_backlog_put(listener);
  tcp_sock_meta(ctx, sock)->listener_id = ID_INVALID;
}

int tcp_listener_ready_push(struct tcp_slow_context *ctx, __u32 listener_id,
    __u32 sock_id)
{
  __u32 pos;
  struct tcp_listener_slow *listener;

  listener = &ctx->listeners[listener_id];
  if (!listener->active || listener->ready_used >= listener->backlog_len)
    return -1;

  pos = listener->ready_head + listener->ready_used;
  if (pos >= listener->backlog_len)
    pos -= listener->backlog_len;
  listener->ready_sids[pos] = sock_id;
  listener->ready_used++;

  return 0;
}

struct tcp_sock *tcp_listener_ready_pop(struct tcp_slow_context *ctx,
    __u32 listener_id)
{
  __u32 sid;
  struct tcp_listener_slow *listener;
  struct tcp_sock *sock;

  listener = &ctx->listeners[listener_id];
  while (listener->active && listener->ready_used != 0)
  {
    sid = listener->ready_sids[listener->ready_head];
    listener->ready_sids[listener->ready_head] = ID_INVALID;
    listener->ready_head++;
    if (listener->ready_head >= listener->backlog_len)
      listener->ready_head = 0;
    listener->ready_used--;

    if (sid == ID_INVALID)
      continue;

    sock = &tcp_sock_map(ctx)[sid];
    if (tcp_sock_meta(ctx, sock)->listener_id != listener_id)
      continue;
    if (sock->state != TCP_SOCK_STATE_ACCEPT_PENDING)
    {
      tcp_sock_meta(ctx, sock)->listener_id = ID_INVALID;
      listener_backlog_put(listener);
      continue;
    }

    return sock;
  }

  return NULL;
}

void tcp_listener_abort_children(struct tcp_slow_context *ctx,
    struct tcp_sock *listen_sock)
{
  __u32 i;
  __u32 listener_id;
  struct tcp_sock *sock;

  listener_id = listen_sock->id;
  for (i = 0; i < ctx->n_socks; i++)
  {
    sock = &tcp_sock_map(ctx)[i];
    if (tcp_sock_meta(ctx, sock)->listener_id != listener_id)
      continue;
    if (sock->state == TCP_SOCK_STATE_CLOSED)
      continue;

    if (sock->remote_port != 0)
      tcp_ctl_tx(ctx, sock, TAS_TCP_RST | TAS_TCP_ACK);
    tcp_sock_close_final(ctx, sock);
  }
}

/*** Listener Helpers *********************************************************/

static void listener_backlog_put(struct tcp_listener_slow *listener)
{
  if (listener->backlog_used > 0)
    listener->backlog_used--;
}

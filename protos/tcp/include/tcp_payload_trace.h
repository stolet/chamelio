#ifndef TCP_PAYLOAD_TRACE_H_
#define TCP_PAYLOAD_TRACE_H_

#include <linux/types.h>
#include <stddef.h>

#define TCP_PAYLOAD_TRACE_MAGIC 0x4b565354U
#define TCP_PAYLOAD_TRACE_VERSION 2
#define TCP_PAYLOAD_TRACE_MAX_EVENTS 48

enum tcp_payload_trace_event_type {
  TCP_PAYLOAD_TRACE_TCP_APP_TX_ACCEPTED = 16,
  TCP_PAYLOAD_TRACE_TCP_RX_DELIVERED = 17,
  TCP_PAYLOAD_TRACE_TCP_TX_SCHED_SENT = 18,
  TCP_PAYLOAD_TRACE_TCP_ACK_PROCESSED = 19,
  TCP_PAYLOAD_TRACE_TCP_INVALID_ACK = 20,
  TCP_PAYLOAD_TRACE_TCP_DUPACK_THRESHOLD = 21,
  TCP_PAYLOAD_TRACE_TCP_OUT_OF_ORDER = 22,
  TCP_PAYLOAD_TRACE_TCP_DUP_PAYLOAD = 23,
  TCP_PAYLOAD_TRACE_TCP_FLOW_BLOCKED = 24,
  TCP_PAYLOAD_TRACE_TCP_QUEUE_FULL = 25,
};

#define TCP_PAYLOAD_TRACE_WHERE_CLIENT_TCP 2
#define TCP_PAYLOAD_TRACE_WHERE_SERVER_TCP 4
#define TCP_PAYLOAD_TRACE_WHERE_CHAMELIO_TCP 5

struct tcp_payload_trace_event {
  __u64 tsc;
  __u8 type;
  __u8 where;
  __s16 sock;
  __u16 arg0;
  __u16 arg1;
} __attribute__((packed));

struct tcp_payload_trace {
  __u32 magic;
  __u16 orig_bodylen;
  __u8 count;
  __u8 capacity;
  __u8 version;
  __u8 flags;
  __u16 reserved;
  __u32 opaque;
  __u64 trace_id;
  struct tcp_payload_trace_event events[TCP_PAYLOAD_TRACE_MAX_EVENTS];
} __attribute__((packed));

static inline __u64 tcp_payload_trace_rdtsc(void)
{
  __u32 eax, edx;
  asm volatile ("rdtsc" : "=a" (eax), "=d" (edx));
  return ((__u64) edx << 32) | eax;
}

static inline __u32 tcp_payload_trace_bodylen(const __u8 *msg)
{
  return ((__u32) msg[8] << 24) | ((__u32) msg[9] << 16) |
      ((__u32) msg[10] << 8) | (__u32) msg[11];
}

static inline struct tcp_payload_trace *tcp_payload_trace_find_msg(
    void *buf, size_t len)
{
  __u8 *msg = (__u8 *) buf;
  __u32 bodylen;
  size_t msg_len;
  struct tcp_payload_trace *tr;

  if (len < 24 + sizeof(*tr))
    return NULL;
  if (msg[0] != 0x80 && msg[0] != 0x81)
    return NULL;

  bodylen = tcp_payload_trace_bodylen(msg);
  msg_len = 24 + (size_t) bodylen;
  if (msg_len > len || bodylen < sizeof(*tr))
    return NULL;

  tr = (struct tcp_payload_trace *) (msg + msg_len - sizeof(*tr));
  if (tr->magic != TCP_PAYLOAD_TRACE_MAGIC ||
      tr->version != TCP_PAYLOAD_TRACE_VERSION ||
      tr->capacity != TCP_PAYLOAD_TRACE_MAX_EVENTS)
    return NULL;
  if ((__u32) tr->orig_bodylen + sizeof(*tr) != bodylen)
    return NULL;

  return tr;
}

static inline void tcp_payload_trace_add_where(struct tcp_payload_trace *tr,
    __u8 type, __u8 where, int sock, __u16 arg0, __u16 arg1)
{
  struct tcp_payload_trace_event *ev;

  if (tr == NULL || tr->magic != TCP_PAYLOAD_TRACE_MAGIC ||
      tr->count >= tr->capacity || tr->count >= TCP_PAYLOAD_TRACE_MAX_EVENTS)
    return;

  ev = &tr->events[tr->count++];
  ev->tsc = tcp_payload_trace_rdtsc();
  ev->type = type;
  ev->where = where;
  ev->sock = (sock < -32768 || sock > 32767) ? -1 : (__s16) sock;
  ev->arg0 = arg0;
  ev->arg1 = arg1;
}

static inline __u8 tcp_payload_trace_infer_where(const __u8 *msg, __u8 type)
{
  int is_request = msg[0] == 0x80;

  switch (type) {
    case TCP_PAYLOAD_TRACE_TCP_APP_TX_ACCEPTED:
    case TCP_PAYLOAD_TRACE_TCP_TX_SCHED_SENT:
    case TCP_PAYLOAD_TRACE_TCP_FLOW_BLOCKED:
      return is_request ? TCP_PAYLOAD_TRACE_WHERE_CLIENT_TCP :
          TCP_PAYLOAD_TRACE_WHERE_SERVER_TCP;

    case TCP_PAYLOAD_TRACE_TCP_RX_DELIVERED:
    case TCP_PAYLOAD_TRACE_TCP_ACK_PROCESSED:
    case TCP_PAYLOAD_TRACE_TCP_INVALID_ACK:
    case TCP_PAYLOAD_TRACE_TCP_DUPACK_THRESHOLD:
    case TCP_PAYLOAD_TRACE_TCP_OUT_OF_ORDER:
    case TCP_PAYLOAD_TRACE_TCP_DUP_PAYLOAD:
    case TCP_PAYLOAD_TRACE_TCP_QUEUE_FULL:
      return is_request ? TCP_PAYLOAD_TRACE_WHERE_SERVER_TCP :
          TCP_PAYLOAD_TRACE_WHERE_CLIENT_TCP;

    default:
      return TCP_PAYLOAD_TRACE_WHERE_CHAMELIO_TCP;
  }
}

static inline void tcp_payload_trace_add(struct tcp_payload_trace *tr,
    __u8 type, int sock, __u16 arg0, __u16 arg1)
{
  tcp_payload_trace_add_where(tr, type, TCP_PAYLOAD_TRACE_WHERE_CHAMELIO_TCP,
      sock, arg0, arg1);
}

static inline void tcp_payload_trace_add_to_msg(void *buf, size_t len,
    __u8 type, int sock, __u16 arg0, __u16 arg1)
{
  struct tcp_payload_trace *tr = tcp_payload_trace_find_msg(buf, len);

  tcp_payload_trace_add_where(tr, type,
      tr == NULL ? TCP_PAYLOAD_TRACE_WHERE_CHAMELIO_TCP :
          tcp_payload_trace_infer_where((const __u8 *) buf, type),
      sock, arg0, arg1);
}

#endif

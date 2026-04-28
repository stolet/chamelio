# UDP Library API

Header:

```c
#include <chamelio/udp_lib.h>
```

Library:

```bash
pkg-config --cflags --libs cham_udp
```

## Initialization

```c
int udp_connect_slow(void);
```

Connect the process to the UDP slow path. Call once before creating contexts.

```c
struct udp_context_lib *udp_ctx_new(void);
```

Create a per-thread UDP context with application/slow-path queues and
fast-path queues.

## Wait Objects

```c
struct udp_wait *udp_wait_new(struct udp_context_lib *ctx);
void udp_wait_free(struct udp_wait *wait);
int udp_wait_add(struct udp_wait *wait, int sockfd, __u32 events, __u64 data);
int udp_wait_mod(struct udp_wait *wait, int sockfd, __u32 events, __u64 data);
int udp_wait_del(struct udp_wait *wait, int sockfd);
```

Events:

| Event | Meaning |
| --- | --- |
| `UDP_WAIT_IN` | socket has readable data. |
| `UDP_WAIT_OUT` | socket has transmit space. |
| `UDP_WAIT_SOCKET` | socket creation completed. |
| `UDP_WAIT_BIND` | bind completed. |
| `UDP_WAIT_SETOPT` | setsockopt completed. |

Flags:

| Flag | Meaning |
| --- | --- |
| `UDP_WAIT_NONBLOCK` | return immediately if no event is ready. |

```c
int udp_wait(struct udp_wait *wait, struct udp_wait_event *events,
    int maxevents, int flags);
int udp_wait_fast(struct udp_wait *wait, struct udp_wait_event *events,
    int maxevents, int flags);
int udp_wait_slow(struct udp_wait *wait, struct udp_wait_event *events,
    int maxevents, int flags);
```

`udp_wait()` checks both fast-path and slow-path events. The fast and slow
variants restrict polling to one side.

## Polling

```c
int udp_poll_slow(struct udp_context_lib *ctx);
int udp_poll_fast(struct udp_context_lib *ctx);
```

Poll message queues and update library-visible socket state.

## Socket Operations

```c
int udp_socket(struct udp_context_lib *ctx);
int udp_bind(struct udp_context_lib *ctx, int sockfd,
    const struct sockaddr *addr, socklen_t addrlen);
int udp_setsockopt(struct udp_context_lib *ctx, int sockfd, __u8 opt);
```

Create a socket, bind it to a local address, or set a supported option such as
`SO_REUSEPORT`.

```c
int udp_sendto(struct udp_context_lib *ctx, int sockfd,
    const void *buf, size_t len,
    const struct sockaddr *addr, socklen_t addr_len);
int udp_recvfrom(struct udp_context_lib *ctx, int sockfd,
    void *buf, size_t len,
    struct sockaddr *addr, socklen_t addr_len);
```

Send and receive UDP payloads. These calls are nonblocking in normal Chamelio
usage; use wait events and polling to drive progress.

## Usage Notes

- Start Chamelio before `udp_slow`.
- Start `udp_slow` before the application calls `udp_connect_slow()`.
- Use one context per application thread.
- Use the guest inner IP in VM deployments.
- Poll fast-path queues frequently in high-throughput loops.

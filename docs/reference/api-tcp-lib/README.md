# TCP Library API

Header:

```c
#include <chamelio/tcp_lib.h>
```

Library:

```bash
pkg-config --cflags --libs cham_tcp
```

## Initialization

```c
int tcp_connect_slow(void);
struct tcp_context_lib *tcp_ctx_new(void);
```

Connect the process to the TCP slow path, then create one context per
application thread.

## Wait Objects

```c
struct tcp_wait *tcp_wait_new(struct tcp_context_lib *ctx);
void tcp_wait_free(struct tcp_wait *wait);
int tcp_wait_add(struct tcp_wait *wait, int sockfd, __u32 events,
    __u64 data);
int tcp_wait_mod(struct tcp_wait *wait, int sockfd, __u32 events,
    __u64 data);
int tcp_wait_del(struct tcp_wait *wait, int sockfd);
```

Events:

| Event | Meaning |
| --- | --- |
| `TCP_WAIT_IN` | socket has readable data. |
| `TCP_WAIT_OUT` | socket has transmit space. |
| `TCP_WAIT_ACCEPT` | listener has a connection ready to accept. |
| `TCP_WAIT_CONNECTED` | nonblocking connect completed. |
| `TCP_WAIT_SOCKET` | socket creation completed. |
| `TCP_WAIT_BIND` | bind completed. |
| `TCP_WAIT_SETOPT` | setsockopt completed. |
| `TCP_WAIT_OP` | connect, listen, or accept operation status changed. |
| `TCP_WAIT_CLOSED` | socket closed. |

Flags:

| Flag | Meaning |
| --- | --- |
| `TCP_WAIT_NONBLOCK` | return immediately if no event is ready. |

```c
int tcp_wait(struct tcp_wait *wait, struct tcp_wait_event *events,
    int maxevents, int flags);
int tcp_wait_fast(struct tcp_wait *wait, struct tcp_wait_event *events,
    int maxevents, int flags);
int tcp_wait_slow(struct tcp_wait *wait, struct tcp_wait_event *events,
    int maxevents, int flags);
```

`tcp_wait()` checks both fast-path and slow-path events.

## Polling

```c
int tcp_poll_slow(struct tcp_context_lib *ctx);
int tcp_poll_fast(struct tcp_context_lib *ctx);
```

Poll message queues and update library-visible socket state.

## Socket Lifecycle

```c
int tcp_socket(struct tcp_context_lib *ctx);
int tcp_bind(struct tcp_context_lib *ctx, int sockfd,
    const struct sockaddr *addr, socklen_t addrlen);
int tcp_setsockopt(struct tcp_context_lib *ctx, int sockfd, __u8 opt);
int tcp_close(struct tcp_context_lib *ctx, int sockfd);
```

Create, bind, configure, and close sockets.

## Active Open

```c
int tcp_connect(struct tcp_context_lib *ctx, int sockfd,
    const struct sockaddr *addr, socklen_t addrlen, int flags);
```

Use `SOCK_NONBLOCK` for nonblocking connect. Wait for `TCP_WAIT_CONNECTED` and
then switch the wait interest to `TCP_WAIT_IN | TCP_WAIT_OUT`.

## Passive Open

```c
int tcp_listen(struct tcp_context_lib *ctx, int sockfd, int backlog);
int tcp_accept(struct tcp_context_lib *ctx, int sockfd,
    struct sockaddr *addr, socklen_t *addrlen, int flags);
```

Wait for `TCP_WAIT_ACCEPT` on the listener before accepting.

## Data Path

```c
int tcp_sendto(struct tcp_context_lib *ctx, int sockfd,
    const void *buf, size_t len,
    const struct sockaddr *addr, socklen_t addr_len);
int tcp_recvfrom(struct tcp_context_lib *ctx, int sockfd,
    void *buf, size_t len,
    struct sockaddr *addr, socklen_t addr_len);
```

The address arguments are present for API similarity with UDP. Connected TCP
use normally passes `NULL, 0`.

```c
int tcp_shutdown(struct tcp_context_lib *ctx, int sockfd, int how);
```

Shut down a TCP socket direction.

## Usage Notes

- Start Chamelio before `tcp_slow`.
- Start `tcp_slow` before the application calls `tcp_connect_slow()`.
- Poll both slow-path and fast-path queues in the event loop.
- Use one context per application thread.
- Use the guest inner IP in VM deployments.

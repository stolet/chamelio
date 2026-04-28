# Use TCP in an Application

Use `libcham_tcp` when an application should open TCP connections or accept TCP
connections through Chamelio.

## Link the Library

With Meson:

```meson
project('my_tcp_app', 'c')

threads_dep = dependency('threads')
cham_tcp_dep = dependency('cham_tcp')

executable(
  'my_tcp_app',
  'main.c',
  dependencies: [threads_dep, cham_tcp_dep],
)
```

With pkg-config:

```bash
cc main.c -o my_tcp_app $(pkg-config --cflags --libs cham_tcp) -pthread
```

## Initialize the Process

Call `tcp_connect_slow()` once before creating contexts. The TCP slow path must
already be running.

```c
#include <chamelio/tcp_lib.h>

if (tcp_connect_slow() != 0)
  return 1;
```

Create one `tcp_context_lib` per worker thread:

```c
struct tcp_context_lib *ctx;
int fd;

ctx = tcp_ctx_new();
if (ctx == NULL)
  return 1;

fd = tcp_socket(ctx);
if (fd < 0)
  return 1;
```

## Accept Connections

```c
struct sockaddr_in addr = {
  .sin_family = AF_INET,
  .sin_port = htons(1234),
};

inet_pton(AF_INET, "10.0.0.1", &addr.sin_addr);

tcp_setsockopt(ctx, fd, SO_REUSEPORT);
tcp_bind(ctx, fd, (struct sockaddr *) &addr, sizeof(addr));
tcp_listen(ctx, fd, 128);
```

Wait for `TCP_WAIT_ACCEPT`, then call `tcp_accept()`:

```c
struct tcp_wait *wait = tcp_wait_new(ctx);
struct tcp_wait_event ev;

tcp_wait_add(wait, fd, TCP_WAIT_ACCEPT, 0);

tcp_poll_slow(ctx);
if (tcp_wait(wait, &ev, 1, 0) > 0 && (ev.events & TCP_WAIT_ACCEPT)) {
  int conn_fd = tcp_accept(ctx, fd, NULL, NULL, SOCK_NONBLOCK);
}
```

## Open Connections

```c
struct sockaddr_in peer = {
  .sin_family = AF_INET,
  .sin_port = htons(1234),
};

inet_pton(AF_INET, "10.0.0.1", &peer.sin_addr);

if (tcp_connect(ctx, fd, (struct sockaddr *) &peer, sizeof(peer),
    SOCK_NONBLOCK) < 0 && errno == EINPROGRESS) {
  tcp_wait_add(wait, fd, TCP_WAIT_CONNECTED, 0);
}
```

After `TCP_WAIT_CONNECTED`, switch the wait interest to read and write:

```c
tcp_wait_mod(wait, fd, TCP_WAIT_IN | TCP_WAIT_OUT, 0);
```

## Send and Receive

```c
char buf[64];

tcp_poll_fast(ctx);
tcp_poll_slow(ctx);

tcp_sendto(ctx, fd, buf, sizeof(buf), NULL, 0);
tcp_recvfrom(ctx, fd, buf, sizeof(buf), NULL, 0);
```

Poll both fast-path and slow-path queues in the application's event loop.
The full API is listed in [TCP library API](../../reference/api-tcp-lib/README.md).

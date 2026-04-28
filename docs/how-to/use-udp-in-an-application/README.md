# Use UDP in an Application

Use `libcham_udp` when an application should send and receive UDP traffic
through Chamelio.

## Link the Library

With Meson:

```meson
project('my_udp_app', 'c')

threads_dep = dependency('threads')
cham_udp_dep = dependency('cham_udp')

executable(
  'my_udp_app',
  'main.c',
  dependencies: [threads_dep, cham_udp_dep],
)
```

With pkg-config:

```bash
cc main.c -o my_udp_app $(pkg-config --cflags --libs cham_udp) -pthread
```

## Initialize the Process

Call `udp_connect_slow()` once before creating contexts. The UDP slow path must
already be running.

```c
#include <chamelio/udp_lib.h>

if (udp_connect_slow() != 0)
  return 1;
```

## Create One Context per Application Thread

The benchmark examples use one `udp_context_lib` per worker thread.

```c
struct udp_context_lib *ctx;
int fd;

ctx = udp_ctx_new();
if (ctx == NULL)
  return 1;

fd = udp_socket(ctx);
if (fd < 0)
  return 1;
```

## Bind When Receiving

```c
struct sockaddr_in addr = {
  .sin_family = AF_INET,
  .sin_port = htons(1234),
};

inet_pton(AF_INET, "10.0.0.1", &addr.sin_addr);

if (udp_bind(ctx, fd, (struct sockaddr *) &addr, sizeof(addr)) != 0)
  return 1;
```

Use the host Chamelio IP on bare metal. Use the guest inner IP when the
application runs in a VM.

## Send and Receive

```c
ssize_t n;
char buf[64];
struct sockaddr_in peer;

n = udp_sendto(ctx, fd, buf, sizeof(buf),
    (struct sockaddr *) &peer, sizeof(peer));

udp_poll_fast(ctx);
n = udp_recvfrom(ctx, fd, buf, sizeof(buf), NULL, 0);
```

`udp_sendto()` and `udp_recvfrom()` return a positive byte count on progress.
They may return `0` or a negative value when no data or no space is available.

## Wait for Events

Use a wait object to avoid spinning only on reads and writes:

```c
struct udp_wait *wait;
struct udp_wait_event ev;

wait = udp_wait_new(ctx);
udp_wait_add(wait, fd, UDP_WAIT_IN | UDP_WAIT_OUT, 0);

udp_poll_fast(ctx);
if (udp_wait_fast(wait, &ev, 1, UDP_WAIT_NONBLOCK) > 0) {
  if (ev.events & UDP_WAIT_IN) {
    /* receive */
  }
  if (ev.events & UDP_WAIT_OUT) {
    /* send */
  }
}
```

The full API is listed in [UDP library API](../../reference/api-udp-lib/README.md).

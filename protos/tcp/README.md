# TCP Protocol

The TCP implementation is a Chamelio protocol with an application library,
slow path, and eBPF fast path.

## Layout

- `include/tcp.h`: shared TCP socket, listener, flow, and control structures.
- `include/tcp_queue_types.h`: messages exchanged between the TCP library and
  slow path.
- `config/`: slow-path command-line parsing and congestion-control defaults.
- `slow/`: TCP slow path, listener handling, socket state, RX control handling,
  and timeout/congestion-control logic.
- `fast/`: TCP eBPF fast path.
- `lib/`: application-facing TCP library.

## Runtime Order

Start Chamelio first, then start `build/protos/tcp/slow/tcp_slow`, then start
applications linked with `libcham_tcp`.

Use `tcp_slow --virt` when the slow path runs inside a Chamelio VM.

## Application API

Applications include:

```c
#include <chamelio/tcp_lib.h>
```

They connect to the slow path with `tcp_connect_slow()`, create one
`tcp_context_lib` per application thread, then use socket-like functions such
as `tcp_socket()`, `tcp_bind()`, `tcp_connect()`, `tcp_listen()`,
`tcp_accept()`, `tcp_sendto()`, `tcp_recvfrom()`, and `tcp_wait()`.

See [the TCP API reference](../../docs/reference/api-tcp-lib/README.md).

# UDP Protocol

The UDP implementation is a Chamelio protocol with an application library,
slow path, and eBPF fast path.

## Layout

- `include/udp.h`: shared UDP socket and map structures.
- `include/udp_queue_types.h`: messages exchanged between the UDP library and
  slow path.
- `config/`: slow-path command-line parsing and defaults.
- `slow/`: UDP slow path and application registration.
- `fast/`: UDP eBPF variants, including block-sized variants selected by
  `udp_slow --block`.
- `lib/`: application-facing UDP library.

## Runtime Order

Start Chamelio first, then start `build/protos/udp/slow/udp_slow`, then start
applications linked with `libcham_udp`.

Use `udp_slow --virt` when the slow path runs inside a Chamelio VM.

## Application API

Applications include:

```c
#include <chamelio/udp_lib.h>
```

They connect to the slow path with `udp_connect_slow()`, create one
`udp_context_lib` per application thread, then use socket-like functions such
as `udp_socket()`, `udp_bind()`, `udp_sendto()`, `udp_recvfrom()`, and
`udp_wait_fast()`.

See [the UDP API reference](../../docs/reference/api-udp-lib/README.md).

# Libraries

Chamelio installs shared libraries, public headers, and pkg-config metadata.

## Installed Libraries

| Library | pkg-config name | Main use |
| --- | --- | --- |
| `libcham.so` | `cham` | Protocol slow paths registering with Chamelio. |
| `libcham_utils.so` | `cham_utils` | Shared queue, utility, and support code. |
| `libcham_udp.so` | `cham_udp` | Applications using the UDP protocol. |
| `libcham_tcp.so` | `cham_tcp` | Applications using the TCP protocol. |

For external applications, use `cham_udp` or `cham_tcp`; they pull in the
Chamelio library dependency they need.

## Installed Headers

After `sudo ninja -C build install`, public headers are installed under the
`chamelio` include directory:

```text
chamelio/cham_lib.h
chamelio/cham_fast.h
chamelio/udp_lib.h
chamelio/tcp_lib.h
chamelio/utils.h
chamelio/queue.h
chamelio/queue_types.h
chamelio/queue_fns.h
```

## Meson Use

UDP application:

```meson
cham_udp_dep = dependency('cham_udp')

executable('app', 'main.c', dependencies: [cham_udp_dep])
```

TCP application:

```meson
cham_tcp_dep = dependency('cham_tcp')

executable('app', 'main.c', dependencies: [cham_tcp_dep])
```

Protocol slow path:

```meson
cham_dep = dependency('cham')

executable('my_slow', 'my_slow.c', dependencies: [cham_dep])
```

## C Include Use

```c
#include <chamelio/udp_lib.h>
#include <chamelio/tcp_lib.h>
#include <chamelio/cham_lib.h>
```

Some in-tree code includes headers without the `chamelio/` prefix because the
local Meson include path points directly at the header directory. External code
should use the installed `chamelio/` prefix.

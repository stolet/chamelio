# Chamelio Libraries

`lib/` contains libraries used by protocol slow paths and application-facing
protocol libraries.

## `libcham`

The Chamelio protocol library is defined by `lib/include/cham_lib.h` and
implemented mainly in `lib/guest.c` and `lib/ivshmem.c`.

Protocol slow paths use it to:

- connect to Chamelio as a bare-metal guest with `cham_connect_guest()`;
- connect from a VM with `cham_new_proto_virt()`;
- register a protocol with `cham_new_proto_bare()` or `cham_new_proto_virt()`;
- create shared-memory queues with `cham_new_queue()`;
- create shared-memory maps with `cham_new_map()`;
- enable queues on fast-path cores with `cham_enable_queue()`;
- upload eBPF bytecode with `cham_upload_ebpf()`.

See [the Chamelio protocol API reference](../docs/reference/api-cham-lib/README.md).

## `libcham_utils`

The utility library is assembled from `lib/utils/`. It includes queue helpers,
shared-memory allocation, schedulers, clocks, timeout management, Unix socket
helpers, packet header definitions, logging, and VFIO support.

The installed public utility headers are:

- `chamelio/utils.h`
- `chamelio/queue.h`
- `chamelio/queue_types.h`
- `chamelio/queue_fns.h`

See [the utility API reference](../docs/reference/api-cham-utils/README.md).

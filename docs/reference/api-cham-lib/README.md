# Chamelio Protocol API

Header:

```c
#include <chamelio/cham_lib.h>
```

This API is for protocol slow paths. Applications normally use a
protocol-specific library such as `cham_udp` or `cham_tcp`.

## Connection and Protocol Registration

```c
int cham_init_ivshmem(void);
```

Mock a QEMU ivshmem client for bare-metal setup.
This is used by the library path that connects a VM guest to host Chamelio.

```c
struct guest_lib *cham_connect_guest(void);
```

Connect a bare-metal guest process to Chamelio through
`/run/chamelio/guest_socket`.

```c
struct proto_lib *cham_new_proto_bare(struct guest_lib *g, __u8 proto_type);
struct proto_lib *cham_new_proto_virt(__u8 proto_type);
```

Register a protocol and map its shared memory region. Use the bare function
for host processes and the virt function inside a VM guest.

## Queues and Maps

```c
struct proto_queue_lib *cham_new_queue(struct proto_lib *p,
    __u32 nelems, __u32 elsize);
```

Create a protocol queue in the protocol shared memory region.

```c
struct proto_map_lib *cham_new_map(struct proto_lib *p,
    __u32 nelems, __u32 elsize);
```

Create a fixed-size protocol map in shared memory. Fast-path code can access
registered maps through `struct cham_ebpf_ctx`.

```c
int cham_enable_queue(struct proto_lib *p, __u16 qid, __u16 core);
int cham_disable_queue(struct proto_lib *p, __u16 qid, __u16 core);
```

Enable or disable a protocol queue on a fast-path core.

## eBPF Upload

```c
struct proto_ebpf_lib *cham_allocate_ebpf(struct proto_lib *p, __u32 size);
int cham_upload_ebpf(struct proto_lib *p, void *ebpf_bytecode, __u32 size);
int cham_free_ebpf(struct proto_lib *p);
```

Allocate shared-memory storage for protocol fast-path bytecode, upload it to
Chamelio, and free it when no longer needed.

## Control Polling

```c
int cham_poll_control(struct proto_lib *p);
```

Poll the control-plane response queue for a protocol. The higher-level
allocation helpers call this while waiting for their own responses.

## Important Structures

`struct proto_lib` holds the shared memory base, allocator, fast-path core
count, local IP, control queues, protocol queues, maps, and eBPF metadata for a
registered protocol.

`struct proto_queue_lib` describes a queue by ID, element count, element size,
and shared-memory offset.

`struct proto_map_lib` describes a map by ID, element count, element size, and
shared-memory offset.

`struct cham_ebpf_ctx` is defined in `cham_fast.h` and is passed to protocol
fast-path handlers.

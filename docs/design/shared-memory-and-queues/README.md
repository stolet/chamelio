# Shared Memory and Queues

Chamelio uses shared memory to let the host fast path, protocol slow paths, and
applications exchange data without copying through the kernel networking stack.

## Shared Memory Regions

Chamelio creates internal shared memory for its own fast/control queues and a
guest protocol shared memory region for registered protocols.

On bare metal, the slow path receives and maps this region after connecting to
Chamelio. In a VM, QEMU exposes the shared region through ivshmem and the guest
maps it through VFIO.

Protocol slow paths allocate maps and queues inside the protocol region with
`cham_new_map()` and `cham_new_queue()`.

## Queues

Queues are single-producer/single-consumer views over shared memory:

- `struct equeue` is the enqueueing view;
- `struct dqueue` is the dequeueing view.

The enqueueing side writes an entry at `queue_tail()`, then publishes it with
`queue_enqueue()`. The dequeueing side reads an entry at `queue_head()`, then
releases it with `queue_dequeue()`.

The first byte of each entry is the queue entry type. `QUEUE_EMPTY` means the
slot is available.

## Maps

Maps are fixed arrays in protocol shared memory. Chamelio registers map
metadata with the fast path so protocol eBPF code can access them through
`struct cham_ebpf_ctx`.

UDP uses maps for socket and port state. TCP uses maps for sockets, listener
ports, flow lookup buckets, and shared control configuration.

## Bump Queues

The existing protocol libraries use bump queues to notify the fast path or app
side that a ring buffer changed. For example, after an application consumes
receive bytes or appends transmit bytes, it bumps the relevant side so the
other side can update availability.

This pattern keeps high-volume payload bytes in shared buffers and sends small
notifications through queues.

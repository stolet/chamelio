# Architecture

Chamelio separates packet processing into a host network stack, protocol slow
paths, protocol application libraries, and applications.

## Major Pieces

`chamelio/` builds the host process. It owns the NIC, DPDK setup, ARP handling,
control plane, fast path, guest registration, and optional network
virtualization.

`lib/` builds the protocol-development library. Protocol slow paths use it to
connect to Chamelio, register a protocol, allocate shared-memory maps and
queues, and upload eBPF fast-path code.

`protos/` contains protocol implementations. UDP and TCP each include a slow
path, fast-path code, shared protocol structures, configuration parsing, and an
application library.

Applications link against a protocol library such as `libcham_udp` or
`libcham_tcp`. They do not call the Chamelio protocol registration API
directly.

## Runtime Relationships

On bare metal, a protocol slow path connects to the Chamelio control plane over
a Unix socket and receives a shared memory region. Applications connect to the
protocol slow path and map that protocol shared memory.

Inside a VM, the host Chamelio process exposes shared memory through QEMU
`ivshmem`. The guest protocol slow path uses the virtualization path
in `libcham` to map the shared memory region and register with host Chamelio.

## Packet Flow

For received packets, Chamelio's fast path parses the packet, finds the
registered protocol, and calls the protocol fast-path handler. The protocol may
deliver payload into application-visible shared memory or punt control work to
the slow path.

For transmitted packets, applications write data through protocol library calls
into shared memory. The protocol fast path dequeues ready work, builds packets,
and transmits them through Chamelio's DPDK NIC path.

The slow path handles operations that are not suitable for the fast path:
registration, socket creation, bind/listen/connect control, protocol state
that needs broader coordination, and control packets.

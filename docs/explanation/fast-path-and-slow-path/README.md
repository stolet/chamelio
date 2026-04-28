# Fast Path and Slow Path

Chamelio protocols are split so common packet work stays in the fast path and
coordination-heavy work stays in the slow path.

## Fast Path

The fast path runs in Chamelio's DPDK packet loop. It is performance-sensitive
and runs protocol code registered by the slow path.

Fast-path protocol code should:

- inspect and update packet data;
- use fixed shared-memory maps;
- enqueue or dequeue protocol work;
- update per-flow or per-socket state that is safe for the fast path;
- stay bounded and verifier-friendly.

Chamelio passes a `struct cham_ebpf_ctx` to protocol fast-path handlers. The
context gives access to packet bytes, shared memory, protocol queues, maps, and
the protocol scheduler.

## Slow Path

The slow path is a normal process. It registers the protocol, allocates shared
state, uploads eBPF bytecode, accepts application connections, and manages
protocol control behavior.

Examples:

- UDP slow path handles application socket creation, bind, and option
  messages.
- TCP slow path handles listen, accept, connect, retransmission timers, control
  packets, and congestion-control updates.

## Why the Split Exists

The fast path must avoid expensive or unbounded work in the packet loop.
The slow path can afford richer logic and blocking system calls because it is
not on every packet's critical path.

This split lets protocol authors keep the packet data path small while still
implementing full protocol behavior in ordinary C code.

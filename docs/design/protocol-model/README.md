# Protocol Model

A Chamelio protocol spans four parts of the system.

## Shared Protocol State

Protocol headers in `protos/<proto>/include/` define the state shared by slow
path, fast path, and application library. Examples include socket tables,
port tables, flow tables, queue entry types, and fast-path configuration maps.

Any structure read by eBPF code should be explicit about size and packing.

## Slow Path

The slow path owns registration and control behavior. It decides what maps and
queues the protocol needs and initializes shared state.

It uses `libcham` to:

- register the protocol;
- allocate maps;
- allocate queues;
- upload fast-path bytecode;
- enable queues on fast-path cores.

## Fast Path

The fast path implements protocol event handlers. Existing protocols use three
kinds of events:

- receive: handle packets from the NIC;
- scheduler: choose what protocol work is ready to transmit;
- dequeue: turn queued application work into transmitted packets.

## Application Library

The application library is the user-facing API. It should expose protocol
operations without requiring applications to know about Chamelio control queues
or shared-memory offsets.

UDP and TCP expose socket-like APIs because that is natural for those
protocols. A custom protocol can expose a different model if that is clearer
for its users.

## Build Integration

Each protocol should build:

- a slow-path executable;
- one or more fast-path handlers;
- an application library;
- public headers for the application library;
- a pkg-config dependency for external applications.

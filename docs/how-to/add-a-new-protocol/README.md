# Add a New Protocol

Use this guide when you want to add a protocol beside TCP and UDP.

The small version of the workflow is:

1. define protocol IDs, shared structs, queue messages, and maps;
2. write a slow path that registers the protocol with Chamelio;
3. create protocol queues and maps in shared memory;
4. upload a fast-path eBPF object;
5. write an application library;
6. add Meson targets and pkg-config output.

## 1. Choose the Protocol Layout

Follow the existing layout:

```text
protos/myproto/
  include/
  config/
  slow/
  fast/
  lib/
  meson.build
```

Use UDP as the simplest model and TCP when you need connection state,
listeners, timers, or control packets.

## 2. Add a Protocol Type

Protocol types are in `enum cham_proto_type` in `queue_types.h`. Add a new
`CHAM_PROTO_*` value and preserve existing values for TCP and UDP.

The Chamelio fast path dispatches protocol events by protocol type. Add
dispatch entries for your protocol's receive, scheduler, and dequeue handlers
where TCP and UDP are currently wired.

## 3. Define Shared State

Create protocol structures under `protos/myproto/include/`.

Typical shared state:

- socket or flow table entries stored in a Chamelio map;
- port or connection lookup tables;
- queue entry types for app-to-slow and slow-to-app messages;
- fast-path configuration stored in a small map.

Keep every structure used by eBPF packed and explicit about sizes.

## 4. Write the Slow Path

The slow path registers the protocol differently depending on where it runs.

Bare metal:

```c
struct guest_lib *guest;
struct proto_lib *proto;

guest = cham_connect_guest();
proto = cham_new_proto_bare(guest, CHAM_PROTO_MYPROTO);
```

VM:

```c
struct proto_lib *proto;

proto = cham_new_proto_virt(CHAM_PROTO_MYPROTO);
```

Then allocate eBPF storage, upload the bytecode, and create maps and queues:

```c
struct proto_map_lib *map;
struct proto_queue_lib *q;

cham_allocate_ebpf(proto, bytecode_len);
cham_upload_ebpf(proto, bytecode, bytecode_len);

map = cham_new_map(proto, nr_entries, sizeof(struct myproto_entry));
q = cham_new_queue(proto, nr_entries, sizeof(struct myproto_queue_entry));
cham_enable_queue(proto, q->id, 0);
```

Use `cham_poll_control()` after asynchronous requests when you need to wait for
control-plane responses. The existing `cham_new_*` helpers already poll for
their own responses.

## 5. Write the Fast Path

Fast-path eBPF code receives `struct cham_ebpf_ctx *ctx`. Through that context
it can inspect packet bytes, access protocol maps, enqueue into protocol
queues, dequeue scheduled work, and use the protocol scheduler.

Implement the same event shape used by TCP and UDP:

- RX event: classify and consume received packets;
- scheduler event: choose work to transmit;
- dequeue event: convert queued application work into packets.

Keep fast-path code bounded and verifier-friendly. Avoid unbounded loops, avoid
ambiguous packet pointer arithmetic, and check `ctx->pkt_end` before reading or
writing packet data.

## 6. Write the Application Library

The application library should hide Chamelio queues and maps from users. The
UDP and TCP libraries expose socket-like APIs and use one context per thread.

A typical library does this:

- connect to the protocol slow path over a Unix socket;
- map the shared memory region;
- create app-to-slow and slow-to-app queues for each context;
- create fast-path queues for RX/TX bumps;
- expose application functions such as socket creation, bind/connect, send,
  receive, wait, and close.

## 7. Add Build Targets

Add Meson files for:

- the slow-path executable;
- the eBPF object;
- the application library;
- installed public headers;
- a pkg-config dependency, for example `cham_myproto`.

External applications should be able to use:

```meson
cham_myproto_dep = dependency('cham_myproto')
```

## 8. Document the Protocol

Add `protos/myproto/README.md` with:

- runtime order;
- slow-path options;
- application library include and link names;
- the maps and queues your protocol creates;
- the limits a user is likely to hit first.

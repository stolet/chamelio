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

## 5. Write and Compile the Fast Path

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

### What the eBPF Source Looks Like

The eBPF source is a normal C file compiled by `ecc`. It should include the
same shared protocol structures that the slow path uses.

```c
#include <linux/types.h>

#include "cham_fast.h"
#include "queue_fns.h"
#include "myproto.h"

int myproto_event_rx(struct cham_ebpf_ctx *ctx)
{
  __u8 *pkt = ctx->pkt;

  if (pkt + sizeof(struct myproto_hdr) > (__u8 *) ctx->pkt_end)
    return -1;

  /* Parse packet, update maps, or enqueue work for the app/slow path. */
  return 0;
}

int myproto_event_sched(struct cham_ebpf_ctx *ctx)
{
  /* Choose scheduled work that is ready to transmit. */
  return 0;
}

int myproto_event_deq(struct cham_ebpf_ctx *ctx)
{
  /* Convert dequeued app work into packet data. */
  return 0;
}
```

The exact function names must match the dispatch code you add for the new
protocol. TCP and UDP currently expose `*_event_rx`, `*_event_sched`, and
`*_event_deq` functions and are wired into the Chamelio protocol dispatcher.

### Compile the eBPF Object

Use `ecc`, which is installed in the Chamelio container. The existing TCP and
UDP Meson files compile eBPF sources with `custom_target()`.

Direct command shape:

```bash
ecc protos/myproto/fast/myproto_fast.ebpf.c \
  --output-path build/protos/myproto/fast \
  -a=-I$PWD/lib/utils/include \
  -a=-I$PWD/lib/include \
  -a=-I$PWD/lib/utils/queue/include \
  -a=-I$PWD/lib/utils/scheduler/include \
  -a=-I$PWD/lib/utils/pkt_hdrs/include \
  -a=-I$PWD/protos/myproto/include
```

Meson shape:

```meson
ecc = find_program('ecc', required: true)
src = meson.project_source_root()

ebpf_incs = [
  src / 'lib/utils/include',
  src / 'lib/include',
  src / 'lib/utils/queue/include',
  src / 'lib/utils/scheduler/include',
  src / 'lib/utils/pkt_hdrs/include',
  src / 'protos/myproto/include',
]

ecc_inc_args = []
foreach d : ebpf_incs
  ecc_inc_args += ['-a=-I' + d]
endforeach

custom_target(
  'myproto_fast_ebpf',
  input: 'myproto_fast.ebpf.c',
  output: 'myproto_fast_ebpf.o',
  command: [ecc, '@INPUT@', '--output-path', meson.current_build_dir()] + ecc_inc_args,
  build_by_default: true,
)
```

The path opened by the slow path must match the object produced by the build.
Keep the path in your protocol config or shared header aligned with the Meson
output name.

### Upload the eBPF Object

The slow path opens the compiled object, maps it, asks Chamelio for eBPF
storage, and uploads the bytes:

```c
int fd;
struct stat st;
void *bytecode;
struct proto_ebpf_lib *ebpf;

fd = open(ctx->config.ebpf_path, O_RDONLY);
if (fd < 0)
  return -1;

if (fstat(fd, &st) != 0)
  return -1;

ebpf = cham_allocate_ebpf(proto, st.st_size);
if (ebpf == NULL)
  return -1;

bytecode = mmap(NULL, ebpf->size, PROT_READ, MAP_PRIVATE, fd, 0);
if (bytecode == MAP_FAILED)
  return -1;

if (cham_upload_ebpf(proto, bytecode, ebpf->size) != 0)
  return -1;
```

UDP stores the selected eBPF path in its slow-path config, which is why
`udp_slow --block=64` can choose a different UDP fast-path object. TCP uses a
fixed TCP eBPF path today.

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

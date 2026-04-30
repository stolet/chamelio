# Utility API

Installed utility headers:

```c
#include <chamelio/utils.h>
#include <chamelio/queue.h>
#include <chamelio/queue_types.h>
#include <chamelio/queue_fns.h>
```

These headers are the public utility surface installed with Chamelio.

## Utility Macros and Endian Types

`utils.h` defines:

- `MIN(a, b)` and `MAX(a, b)`
- `MEM_BARRIER()`
- `STATIC_ASSERT(COND, MSG)`
- packed big-endian wrapper types `beui16_t`, `beui32_t`, and `beui64_t`
- conversion helpers `f_beui16()`, `f_beui32()`, `f_beui64()`
- conversion helpers `t_beui16()`, `t_beui32()`, `t_beui64()`
- `utils_prefetch0()`

## Queue Limits

`queue.h` defines:

```c
#define MAX_PROTO_QUEUES 128
#define MAX_PROTO_MAPS 8
```

## Queue Structures

```c
struct equeue {
  __u32 tail;
  __u32 nelems;
  size_t elsize;
  __u64 off;
  void *entries;
};

struct dqueue {
  __u32 head;
  __u32 nelems;
  size_t elsize;
  __u64 off;
  void *entries;
};
```

An `equeue` is only used by the enqueueing side. A `dqueue` is only used by
the dequeueing side. This split keeps the shared queue protocol simple.

## Queue Functions

```c
struct equeue *equeue_new(__u32 nelems, size_t elsize,
    void *addr, __u64 off);
struct dqueue *dqueue_new(__u32 nelems, size_t elsize,
    void *addr, __u64 off);
int equeue_init(struct equeue *q, __u32 nelems, size_t elsize,
    void *addr, __u64 off);
int dqueue_init(struct dqueue *q, __u32 nelems, size_t elsize,
    void *addr, __u64 off);
```

Create or initialize enqueue and dequeue views over shared queue memory.

```c
static inline void *queue_tail(struct equeue *q);
static inline int queue_enqueue(struct equeue *q, __u8 type);
static inline void *queue_head(struct dqueue *q);
static inline int queue_dequeue(struct dqueue *q);
```

Typical enqueue pattern:

```c
struct my_entry *e = queue_tail(q);
if (e != NULL)
{
  e->field = value;
  queue_enqueue(q, MY_ENTRY_TYPE);
}
```

Typical dequeue pattern:

```c
struct my_entry *e = queue_head(q);
if (e != NULL)
{
  /* read entry */
  queue_dequeue(q);
}
```

## Protocol and Queue Message Types

`queue_types.h` defines:

- protocol IDs in `enum cham_proto_type`;
- control queue entry types in `enum queue_type`;
- request and response payloads used by `libcham`;
- `struct queue_entry`, the common control queue entry.

Protocol authors usually need the protocol ID enum and the shared queue entry
format when adding new registration behavior.

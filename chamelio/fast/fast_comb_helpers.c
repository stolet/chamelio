#include <stdint.h>
#include <string.h>

#include "clock.h"
#include "ebpf_helpers.h"
#include "queue.h"
#include "queue_types.h"
#include "scheduler.h"
#include "utils.h"

static inline int comb_sched_remove(struct cham_scheduler *sched, __u32 id);

/* TODO: I don't like how this is redefined here but will
   have to figure out later if there is a better way to allow
   for cross module optimisations of inlined functions defined in headers */
static inline int comb_queue_enqueue(struct equeue *q, __u8 type)
{
  __u32 tail;
  __u8 *tail_type;

  tail_type = (__u8 *) q->entries + q->tail;
  if (*tail_type != QUEUE_EMPTY)
    return -1;

  tail = q->tail + q->elsize;
  if (tail >= (q->elsize * q->nelems))
    tail = 0;
  q->tail = tail;

  MEM_BARRIER();
  *tail_type = type;
  return 0;
}

static inline int comb_queue_dequeue(struct dqueue *q)
{
  __u32 head;
  __u8 *type;

  type = (__u8 *) q->entries + q->head;
  if (*type == QUEUE_EMPTY)
    return -1;

  head = q->head + q->elsize;
  if (head >= (q->elsize * q->nelems))
    head = 0;
  q->head = head;

  MEM_BARRIER();
  *type = QUEUE_EMPTY;
  return 0;
}

static inline void *comb_queue_head(struct dqueue *q)
{
  __u8 *type;

  type = (__u8 *) q->entries + q->head;
  if (*type == QUEUE_EMPTY)
    return NULL;

  return type;
}

static inline void *comb_queue_tail(struct equeue *q)
{
  __u8 *type;

  type = (__u8 *) q->entries + q->tail;
  if (*type != QUEUE_EMPTY)
    return NULL;

  return type;
}

static inline void comb_sched_insert(struct cham_scheduler *sched, __u32 id)
{
  __u32 prev, cur;
  struct cham_sched_entry *entry;

  entry = &sched->entries[id];
  if (sched->head == SCHED_ID_INVALID)
  {
    sched->head = id;
    sched->tail = id;
    entry->next_entry = SCHED_ID_INVALID;
    return;
  }

  prev = SCHED_ID_INVALID;
  cur = sched->head;
  while (cur != SCHED_ID_INVALID &&
      sched->entries[cur].priority < entry->priority)
  {
    prev = cur;
    cur = sched->entries[cur].next_entry;
  }

  if (prev == SCHED_ID_INVALID)
  {
    entry->next_entry = sched->head;
    sched->head = id;
    return;
  }

  entry->next_entry = sched->entries[prev].next_entry;
  sched->entries[prev].next_entry = id;
  if (entry->next_entry == SCHED_ID_INVALID)
    sched->tail = id;
}

static inline int comb_sched_add(struct cham_scheduler *sched, __u32 id,
    __u64 priority, __u32 avail)
{
  __u32 total_avail;
  struct cham_sched_entry *entry;

  entry = &sched->entries[id];
  if (entry->id != SCHED_ID_INVALID)
  {
    total_avail = entry->avail + avail;
    if (entry->priority == priority)
    {
      entry->avail = total_avail;
      return 0;
    }

    if (comb_sched_remove(sched, id) != 0)
      return -1;
    entry->id = id;
    entry->next_entry = SCHED_ID_INVALID;
    entry->priority = priority;
    entry->avail = total_avail;
    entry->opaque = 0;
    comb_sched_insert(sched, id);
    return 0;
  }

  entry->id = id;
  entry->next_entry = SCHED_ID_INVALID;
  entry->priority = priority;
  entry->avail = avail;
  entry->opaque = 0;
  comb_sched_insert(sched, id);
  return 0;
}

static inline struct cham_sched_entry *comb_sched_head(
    struct cham_scheduler *sched)
{
  if (sched->head == SCHED_ID_INVALID)
    return NULL;

  return &sched->entries[sched->head];
}

static inline int comb_sched_pop(struct cham_scheduler *sched)
{
  int pop_idx;
  struct cham_sched_entry *pop_entry;

  if (sched->head == SCHED_ID_INVALID)
    return -1;

  pop_idx = sched->head;
  pop_entry = &sched->entries[pop_idx];

  sched->head = pop_entry->next_entry;
  if (sched->head == SCHED_ID_INVALID)
    sched->tail = SCHED_ID_INVALID;

  pop_entry->id = SCHED_ID_INVALID;
  pop_entry->next_entry = SCHED_ID_INVALID;
  pop_entry->priority = 0;
  pop_entry->avail = 0;
  pop_entry->opaque = 0;
  return 0;
}

static inline int comb_sched_remove(struct cham_scheduler *sched, __u32 id)
{
  __u32 prev, cur;
  struct cham_sched_entry *entry;

  prev = SCHED_ID_INVALID;
  cur = sched->head;
  while (cur != SCHED_ID_INVALID && cur != id)
  {
    prev = cur;
    cur = sched->entries[cur].next_entry;
  }
  if (cur == SCHED_ID_INVALID)
    return -1;

  entry = &sched->entries[id];
  if (prev == SCHED_ID_INVALID)
    sched->head = entry->next_entry;
  else
    sched->entries[prev].next_entry = entry->next_entry;

  if (sched->tail == id)
    sched->tail = prev;
  entry->id = SCHED_ID_INVALID;
  entry->next_entry = SCHED_ID_INVALID;
  entry->priority = 0;
  entry->avail = 0;
  entry->opaque = 0;
  return 0;
}

static inline void *comb_map_get(void *map_base)
{
  return map_base;
}

static inline void *comb_map_lookup(void *map_base, __u64 id, __u64 elsize)
{
  return (__u8 *) map_base + (id * elsize);
}

static inline __u64 comb_rdtsc(void)
{
  return clock_rdtsc();
}

static inline __u64 comb_now_us(void)
{
  return clock_tsc_to_us(clock_rdtsc());
}

static inline __u64 comb_rate_delay_tsc(__u32 bytes, __u32 rate_kbps)
{
  __u64 cycles_per_us;
  __u64 num;

  if (bytes == 0 || rate_kbps == 0)
    return 0;

  cycles_per_us = clock_us_to_tsc(1);
  if (cycles_per_us == 0)
    return 0;

  num = (__u64) bytes * 8 * 1000 * cycles_per_us;
  return (num + rate_kbps - 1) / rate_kbps;
}

#define DEFINE_EBPF_HELPER(sym, fn)                                           \
  __u64 fn(__u64 arg1, __u64 arg2, __u64 arg3, __u64 arg4, __u64 arg5)       \
      __asm__(sym);                                                           \
  __u64 fn(__u64 arg1, __u64 arg2, __u64 arg3, __u64 arg4, __u64 arg5)

DEFINE_EBPF_HELPER(EBPF_EXT_HELPER_QUEUE_TAIL, ext_queue_tail)
{
  (void) arg3;
  (void) arg4;
  (void) arg5;
  return (uintptr_t) comb_queue_tail((struct equeue *)(uintptr_t) arg1);
}

DEFINE_EBPF_HELPER(EBPF_EXT_HELPER_QUEUE_ENQUEUE, ext_queue_enqueue)
{
  (void) arg3;
  (void) arg4;
  (void) arg5;
  return (__u64) comb_queue_enqueue((struct equeue *)(uintptr_t) arg1,
      (__u8) arg2);
}

DEFINE_EBPF_HELPER(EBPF_EXT_HELPER_MEMCPY, ext_memcpy)
{
  (void) arg4;
  (void) arg5;
  return (uintptr_t) memcpy((void *)(uintptr_t) arg1,
      (const void *)(uintptr_t) arg2, (size_t) arg3);
}

DEFINE_EBPF_HELPER(EBPF_EXT_HELPER_SCHED_HEAD, ext_sched_head)
{
  (void) arg2;
  (void) arg3;
  (void) arg4;
  (void) arg5;
  return (uintptr_t) comb_sched_head(
      (struct cham_scheduler *)(uintptr_t) arg1);
}

DEFINE_EBPF_HELPER(EBPF_EXT_HELPER_SCHED_POP, ext_sched_pop)
{
  (void) arg2;
  (void) arg3;
  (void) arg4;
  (void) arg5;
  return (__u64) comb_sched_pop((struct cham_scheduler *)(uintptr_t) arg1);
}

DEFINE_EBPF_HELPER(EBPF_EXT_HELPER_SCHED_ADD, ext_sched_add)
{
  (void) arg5;
  return (__u64) comb_sched_add((struct cham_scheduler *)(uintptr_t) arg1,
      (__u32) arg2, (__u64) arg3, (__u32) arg4);
}

DEFINE_EBPF_HELPER(EBPF_EXT_HELPER_MAP_GET, ext_map_get)
{
  (void) arg2;
  (void) arg3;
  (void) arg4;
  (void) arg5;
  return (uintptr_t) comb_map_get((void *)(uintptr_t) arg1);
}

DEFINE_EBPF_HELPER(EBPF_EXT_HELPER_MAP_LOOKUP, ext_map_lookup)
{
  (void) arg4;
  (void) arg5;
  return (uintptr_t) comb_map_lookup((void *)(uintptr_t) arg1,
      (__u64) arg2, (__u64) arg3);
}

DEFINE_EBPF_HELPER(EBPF_EXT_HELPER_QUEUE_HEAD, ext_queue_head)
{
  (void) arg2;
  (void) arg3;
  (void) arg4;
  (void) arg5;
  return (uintptr_t) comb_queue_head((struct dqueue *)(uintptr_t) arg1);
}

DEFINE_EBPF_HELPER(EBPF_EXT_HELPER_QUEUE_DEQUEUE, ext_queue_dequeue)
{
  (void) arg2;
  (void) arg3;
  (void) arg4;
  (void) arg5;
  return (__u64) comb_queue_dequeue((struct dqueue *)(uintptr_t) arg1);
}

DEFINE_EBPF_HELPER(EBPF_EXT_HELPER_RDTSC, ext_rdtsc)
{
  (void) arg1;
  (void) arg2;
  (void) arg3;
  (void) arg4;
  (void) arg5;
  return comb_rdtsc();
}

DEFINE_EBPF_HELPER(EBPF_EXT_HELPER_NOW_US, ext_now_us)
{
  (void) arg1;
  (void) arg2;
  (void) arg3;
  (void) arg4;
  (void) arg5;
  return comb_now_us();
}

DEFINE_EBPF_HELPER(EBPF_EXT_HELPER_RATE_DELAY_TSC, ext_rate_delay_tsc)
{
  (void) arg3;
  (void) arg4;
  (void) arg5;
  return comb_rate_delay_tsc((__u32) arg1, (__u32) arg2);
}

DEFINE_EBPF_HELPER(EBPF_EXT_HELPER_SCHED_REMOVE, ext_sched_remove)
{
  (void) arg3;
  (void) arg4;
  (void) arg5;
  return (__u64) comb_sched_remove((struct cham_scheduler *)(uintptr_t) arg1,
      (__u32) arg2);
}

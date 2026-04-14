#include "stdlib.h"
#include "scheduler_fns.h"
#include "log.h"

static void sched_insert(struct cham_scheduler *sched, __u32 id);

void sched_init(struct cham_scheduler *sched)
{
  __u32 i;

  sched->vtime = 0;
  sched->head = SCHED_ID_INVALID;
  sched->tail = SCHED_ID_INVALID;
  for (i = 0; i < MAX_SCHED_ENTRIES; i++)
  {
    sched->entries[i].id = SCHED_ID_INVALID;
    sched->entries[i].next_entry = SCHED_ID_INVALID;
    sched->entries[i].priority = 0;
    sched->entries[i].avail = 0;
    sched->entries[i].opaque = 0;
  }
}

int sched_add(struct cham_scheduler *sched, __u32 id, __u64 priority,
    __u32 avail)
{
  struct cham_sched_entry *entry;

  entry = &sched->entries[id];
  if (entry->id != SCHED_ID_INVALID)
  {
    entry->avail += avail;
    if (entry->priority == priority)
      return 0;

    sched_remove(sched, id);
    entry->priority = priority;
    sched_insert(sched, id);
    return 0;
  }

  entry->id = id;
  entry->next_entry = SCHED_ID_INVALID;
  entry->priority = priority;
  entry->avail = avail;
  entry->opaque = 0;
  sched_insert(sched, id);

  return 0;
}

struct cham_sched_entry *sched_head(struct cham_scheduler *sched)
{
  if (sched->head == SCHED_ID_INVALID)
    return NULL;

  return &sched->entries[sched->head];
}

int sched_pop(struct cham_scheduler *sched)
{
  int pop_idx;
  struct cham_sched_entry *pop_entry;

  /* Queue is empty */
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

int sched_remove(struct cham_scheduler *sched, __u32 id)
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
  {
    sched->head = entry->next_entry;
  }
  else
  {
    sched->entries[prev].next_entry = entry->next_entry;
  }

  if (sched->tail == id)
    sched->tail = prev;
  entry->id = SCHED_ID_INVALID;
  entry->next_entry = SCHED_ID_INVALID;
  entry->priority = 0;
  entry->avail = 0;
  entry->opaque = 0;

  return 0;
}

static void sched_insert(struct cham_scheduler *sched, __u32 id)
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

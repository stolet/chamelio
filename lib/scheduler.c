#include "cham_fast.h"
#include "cham_scheduler.h"
#include "log.h"

void sched_init(struct cham_scheduler *sched)
{
  sched->head = SCHED_ID_INVALID;
  sched->tail = SCHED_ID_INVALID;
}

int sched_add(struct cham_scheduler *sched, uint32_t id, uint32_t priority, 
    uint32_t avail, uint64_t opaque)
{
  int prev, cur;
  struct cham_sched_entry *entry;

  entry = &sched->entries[id];
  entry->id = id;
  entry->next_entry = SCHED_ID_INVALID;
  entry->priority = priority;
  entry->avail = avail;
  entry->opaque = opaque;
  
  if (sched->head == SCHED_ID_INVALID)
  {
    sched->head = id;
    sched->tail = id;
    return 0;
  }

  prev = SCHED_ID_INVALID;
  cur = sched->head;
  while (cur != SCHED_ID_INVALID && 
      sched->entries[cur].priority >= entry->priority)
  {
    prev = cur;
    cur = sched->entries[cur].next_entry;
  }

  if (prev == SCHED_ID_INVALID)
  {
    /* Insert at head */
    entry->next_entry = sched->head;
    sched->head = id;
  }
  else
  {
    /* Insert in middle or end */
    entry->next_entry = sched->entries[prev].next_entry;
    sched->entries[prev].next_entry = id;
  }

  if (entry->next_entry == SCHED_ID_INVALID)
    sched->tail = id;

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

  return 0;
}
#include "scheduler.h"

void sched_init(struct scheduler *sched)
{
  sched->head = SCHED_ID_INVALID;
  sched->tail = SCHED_ID_INVALID;
  sched->free_head = 0;

  /* Link free entries */
  for (uint32_t i = 0; i < MAX_SCHED_ENTRIES - 1; ++i)
  {
    sched->entries[i].next_entry = i + 1;
    sched->entries[i].id = SCHED_ID_INVALID;
  }

  /* End of free list */
  sched->entries[MAX_SCHED_ENTRIES - 1].next_entry = SCHED_ID_INVALID;
  sched->entries[MAX_SCHED_ENTRIES - 1].id = SCHED_ID_INVALID;
}

int sched_add(struct scheduler *sched, struct sched_entry *entry)
{
  int prev, cur;
  struct sched_entry *new_entry;

  /* No space */
  int new_idx = sched->free_head;
  if (new_idx == SCHED_ID_INVALID)
    return -1;

  new_entry = &sched->entries[new_idx];
  sched->free_head = new_entry->next_entry;

  /* Copy data from the provided entry */
  *new_entry = *entry;
  new_entry->next_entry = SCHED_ID_INVALID;

  if (sched->head == SCHED_ID_INVALID)
  {
    sched->head = new_idx;
    sched->tail = new_idx;
    return 0;
  }

  prev = -1;
  cur = sched->head;
  while (cur != SCHED_ID_INVALID &&
         sched->entries[cur].priority > new_entry->priority)
  {
    prev = cur;
    cur = sched->entries[cur].next_entry;
  }

  if (prev == -1)
  {
    /* Insert at head */
    new_entry->next_entry = sched->head;
    sched->head = new_idx;
  }
  else
  {
    /* Insert in middle or end */
    new_entry->next_entry = sched->entries[prev].next_entry;
    sched->entries[prev].next_entry = new_idx;
  }

  if (new_entry->next_entry == SCHED_ID_INVALID)
    sched->tail = new_idx;

  return 0;
}

int sched_remove(struct scheduler *sched, uint32_t id)
{
  int prev, cur;

  prev = -1;
  cur = sched->head;
  while (cur != SCHED_ID_INVALID)
  {
    if (sched->entries[cur].id == id)
    {
      if (prev == -1)
      {
        sched->head = sched->entries[cur].next_entry;
      }
      else
      {
        sched->entries[prev].next_entry = sched->entries[cur].next_entry;
      }

      if (sched->tail == cur)
        sched->tail = prev;

      /* Return removed entry to free list */
      sched->entries[cur].id = SCHED_ID_INVALID;
      sched->entries[cur].next_entry = sched->free_head;
      sched->free_head = cur;

      return 0;
    }

    prev = cur;
    cur = sched->entries[cur].next_entry;
  }

  return -1;
}

struct sched_entry *sched_head(struct scheduler *sched)
{
  if (sched->head == SCHED_ID_INVALID)
    return NULL;

  return &sched->entries[sched->head];
}

int sched_pop(struct scheduler *sched)
{
  int pop_idx;
  struct sched_entry *pop_entry;

  /* Queue is empty */
  if (sched->head == SCHED_ID_INVALID)
    return -1;

  pop_idx = sched->head;
  pop_entry = &sched->entries[pop_idx];

  sched->head = pop_entry->next_entry;
  if (sched->head == SCHED_ID_INVALID)
    sched->tail = SCHED_ID_INVALID;

  /* Return popped entry to free list */
  pop_entry->id = SCHED_ID_INVALID;
  pop_entry->next_entry = sched->free_head;
  sched->free_head = pop_idx;

  return 0;
}
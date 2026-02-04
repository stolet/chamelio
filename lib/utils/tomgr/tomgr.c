#include "tomgr.h"
#include <stdlib.h>
#include <string.h>

#define PARENT(i) ((i - 1) / 2)
#define LEFT(i) (2 * i + 1)
#define RIGHT(i) (2 * i + 2)

static inline void swap(struct tomgr *mgr,
    struct to_heap_entry *a, struct to_heap_entry *b);
static inline void heapify_down(struct tomgr *mgr, int idx);
static inline void heapify_up(struct tomgr *mgr, int idx);
static inline int find_free_entry(struct tomgr *mgr);

struct tomgr *tomgr_init()
{
  int i;
  struct tomgr *mgr;

  mgr = malloc(sizeof(struct tomgr));
  if (!mgr)
    return NULL;

  mgr->size = 0;
  for (i = 0; i < TOMGR_SIZE; i++)
    mgr->entries[i].heap_idx = TO_INVALID;
  return mgr;
}

struct to_entry *tomgr_insert(struct tomgr *mgr, __u8 type, __u64 to, void *data)
{
  int idx;
  struct to_entry *entry;

  /* Heap is full */
  if (mgr->size >= TOMGR_SIZE)
    return NULL;

  idx = find_free_entry(mgr);
  if (idx < 0)
    return NULL;

  /* Add new entry to the entries array */
  entry = &mgr->entries[idx];
  entry->type = type;
  entry->to = to;
  entry->data = data;
  entry->heap_idx = mgr->size; /* Set heap index */

  /* Add index to the heap */
  mgr->heap[mgr->size].idx = idx;
  mgr->heap[mgr->size].to = to;
  mgr->size++;

  heapify_up(mgr, mgr->size - 1);
  return entry;
}

int tomgr_cancel(struct tomgr *mgr, struct to_entry *e)
{
  int heap_idx = e->heap_idx;

  /* Entry not found or invalid index */
  if (heap_idx < 0 || heap_idx >= mgr->size 
      || &mgr->entries[mgr->heap[heap_idx].idx] != e)
    return -1;

  e->heap_idx = TO_INVALID;
  
  /* If there is only one entry return */
  if (mgr->size == 1)
  {
    mgr->size--;
    return 0;
  }
  
  /* Replace the heap entry with the last element */
  mgr->heap[heap_idx] = mgr->heap[mgr->size - 1];
  mgr->entries[mgr->heap[heap_idx].idx].heap_idx = heap_idx;
  mgr->size--;

  /* Restore heap property */
  if (heap_idx < mgr->size)
  {
    if (heap_idx > 0 &&
        mgr->heap[PARENT(heap_idx)].to > mgr->heap[heap_idx].to)
    {
      heapify_up(mgr, heap_idx);
    }
    else
    {
      heapify_down(mgr, heap_idx);
    }
  }
  return 0;
}

struct to_entry *tomgr_peek(struct tomgr *mgr)
{
  if (mgr->size == 0)
    return NULL;

  /* Return the entry at the root of the heap */
  return &mgr->entries[mgr->heap[0].idx];
}

struct to_entry *tomgr_pop(struct tomgr *mgr)
{
  struct to_entry *earliest;

  if (mgr->size == 0)
    return NULL;

  /* Get the entry at the root of the heap */
  earliest = &mgr->entries[mgr->heap[0].idx];
  earliest->heap_idx = TO_INVALID;

  /* Replace root with the last element */
  mgr->heap[0] = mgr->heap[mgr->size - 1];
  mgr->size--;

  heapify_down(mgr, 0);
  return earliest;
}

static inline void swap(struct tomgr *mgr, 
    struct to_heap_entry *a, struct to_heap_entry *b)
{
  struct to_heap_entry temp;

  /* Swap heap entries */
  temp = *a;
  *a = *b;
  *b = temp;

  /* Update heap_idx in the corresponding to_entry structures */
  mgr->entries[a->idx].heap_idx = a - mgr->heap;
  mgr->entries[b->idx].heap_idx = b - mgr->heap;
}

static inline void heapify_down(struct tomgr *mgr, int idx)
{
  int smallest, left, right;

  smallest = idx;
  left = LEFT(idx);
  right = RIGHT(idx);

  if (left < mgr->size && mgr->heap[left].to < mgr->heap[smallest].to)
    smallest = left;
  if (right < mgr->size && mgr->heap[right].to < mgr->heap[smallest].to)
    smallest = right;

  if (smallest != idx)
  {
    swap(mgr, &mgr->heap[idx], &mgr->heap[smallest]);
    heapify_down(mgr, smallest);
  }
}

static inline void heapify_up(struct tomgr *mgr, int idx)
{
  while (idx > 0 && mgr->heap[PARENT(idx)].to > mgr->heap[idx].to)
  {
    swap(mgr, &mgr->heap[idx], &mgr->heap[PARENT(idx)]);
    idx = PARENT(idx);
  }
}

static inline int find_free_entry(struct tomgr *mgr)
{
  int i;

  for (i = 0; i < TOMGR_SIZE; i++)
  {
    if (mgr->entries[i].heap_idx == TO_INVALID)
      return i;
  }

  return -1;
}

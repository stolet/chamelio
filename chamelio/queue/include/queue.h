#ifndef QUEUE_H_
#define QUEUE_H_

#include <stdlib.h>
#include <stdint.h>

#include "utils.h"
#include "config.h"
#include "shmalloc.h"

enum queue_type {
  QUEUE_EMPTY = 0,
  QUEUE_ERROR,

  QUEUE_ARP_TX,
  QUEUE_ARP_RX,

  QUEUE_NEW_APP,
};

struct queue_new_app {

};

struct queue_entry {
  volatile uint8_t type;
  union {
    struct queue_new_app new_app;
    uint8_t raw[63];
  } __attribute__((packed)) data;
} __attribute__((packed));
STATIC_ASSERT(sizeof(struct queue_entry) == 64, queue_entry_size);


struct queue {
  volatile uint32_t head;
  volatile uint32_t tail;
  uint32_t size;
  struct shm_handle *sh;
  struct queue_entry *entries;
};

struct queue * queue_new(uint32_t size, struct shm_allocator *alloc);
int queue_enqueue(struct queue *q, uint8_t type);
int queue_dequeue(struct queue *q);
struct queue_entry * queue_head(struct queue *q);

#endif

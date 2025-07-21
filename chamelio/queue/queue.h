#ifndef QUEUE_H_
#define QUEUE_H_

#include <stdint.h>

#define QUEUE_TYPE_EMPTY 0
#define QUEUE_TYPE_ARP_TX 1
#define QUEUE_TYPE_ARP_RX 2

#define QUEUE_TYPE_NEW_FLOW 3

#define QUEUE_TYPE_ERROR 4

struct queue {
  volatile uint32_t head;
  volatile uint32_t tail;
  uint32_t size;
  struct queue_entry *entries;
};

struct queue_entry {
  volatile uint8_t type;
};

struct queue * queue_new();
int queue_enqueue(struct queue *q, uint8_t type);
int queue_dequeue(struct queue *q);
struct queue_entry * queue_head(struct queue *q);

#endif

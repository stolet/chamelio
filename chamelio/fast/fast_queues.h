#ifndef FAST_QUEUES_H_
#define FAST_QUEUES_H_

/* Polls all guest queues and runs custom dequeue logic */
int fast_queues_poll(struct fast_context *ctx);

#endif
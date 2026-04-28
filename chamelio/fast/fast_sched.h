#ifndef FAST_SCHED_H_
#define FAST_SCHED_H_

#include "fast.h"

/* Polls each guest and runs custom scheduler logic */
int fast_sched_poll(struct fast_context *ctx);

#endif

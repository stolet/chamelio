#ifndef FAST_TX_H_
#define FAST_TX_H_

#include "fast.h"

/* Polls each guest and runs custom transmit logic */
int fast_tx_poll(struct fast_context *ctx);

#endif
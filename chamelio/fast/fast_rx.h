#ifndef FAST_RX_H_
#define FAST_RX_H_

/* Polls NIC for received packets, identifies guest, runs custom receive logic */
int fast_rx_poll(struct fast_context *ctx);

#endif
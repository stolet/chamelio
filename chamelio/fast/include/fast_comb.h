#ifndef FAST_COMB_H_
#define FAST_COMB_H_

#include <stddef.h>
#include <stdint.h>

uint64_t fast_rx_poll_comb(void *mem, size_t mem_len);
uint64_t fast_queues_poll_comb(void *mem, size_t mem_len);
uint64_t fast_tx_poll_comb(void *mem, size_t mem_len);

#endif

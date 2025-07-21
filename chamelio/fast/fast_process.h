#ifndef FAST_PROCESS_H_
#define FAST_PROCESS_H_

uint8_t fast_process_packet_rx(struct fast_context *ctx, struct rte_mbuf *mb);
uint8_t fast_process_packet_tx(struct fast_context *ctx, struct rte_mbuf *mb);
int fast_process_packet_error(struct fast_context *ctx, 
    struct rte_mbuf *mb, uint8_t type);

#endif


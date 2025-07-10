#ifndef FAST_PROCESS_H_
#define FAST_PROCESS_H_

int fast_process_packet_rx(struct fast_path_context *ctx, struct rte_mbuf *mb);
int fast_process_packet_tx(struct fast_path_context *ctx, struct rte_mbuf *mb);

#endif


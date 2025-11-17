#ifndef UDP_FAST_H_
#define UDP_FAST_H_

#include "cham_fast.h"
#include "queue_types.h"

int udp_init_fp(void *config);
int udp_event_rx(void *pkt, struct cham_proto_handle *handle);
int udp_event_tx(void *pkt, struct cham_proto_handle *handle);
int udp_event_deq(int qid, struct queue_entry *qe, 
    struct cham_proto_handle *handle);

#endif
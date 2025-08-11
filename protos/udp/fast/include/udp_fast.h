#ifndef UDP_FAST_H_
#define UDP_FAST_H_

int udp_event_rx(void *pkt, void *shm);
int udp_event_tx(int tid, void *pkt);
int udp_event_deq(int qid, void *qe);

int udp_act_enq(int qid, void *qe);
int udp_act_txsched(int tid);

#endif
#include "udp_fast.h"

int udp_event_rx(void *pkt, void *shm)
{
  return 0;
}

int udp_event_tx(int tid, void *pkt)
{
  return 0;
}

int udp_event_deq(int qid, void *qe)
{
  return 0;
}

int udp_act_enq(int qid, void *qe)
{
  return 0;
}

int udp_act_txsched(int tid)
{
  return 0;
}
#include <rte_ip4.h>
#include <cham_fast.h>

#include "scheduler_fns.h"
#include "udp_hdr.h"
#include "ip_hdr.h"
#include "eth_hdr.h"
#include "udp.h"
#include "udp_fast.h"
#include "udp_queue_types.h"
#include "queue_fns.h"
#include "queue_types.h"
#include "log.h"

int udp_event_rx(void *pkt, struct cham_proto_handle *handle)
{
  return 0;
}

int udp_event_tx(void *pkt, struct cham_proto_handle *handle)
{
  return 0;
}

int udp_event_deq(int qid, struct queue_entry *qe,
  struct cham_proto_handle *handle)
{
  return 0;
}
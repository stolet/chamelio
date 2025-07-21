#ifndef ETH_H_
#define ETH_H_

#include <rte_mbuf.h>

#include "eth_pkt.h"
#include "../../../utils/log/log.h"
#include "../../../utils/utils.h"
#include "../../queue/queue.h"

struct eth_state {

};

int mac_from_text(const char *text, uint8_t out[ETH_ADDR_LEN])
{
    unsigned int tmp[ETH_ADDR_LEN];

    if (sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x", &tmp[0], &tmp[1], &tmp[2], &tmp[3], &tmp[4], &tmp[5]) != 6)
        return -1;          

    for (size_t i = 0; i < ETH_ADDR_LEN; ++i)
        out[i] = (uint8_t)tmp[i];

    return 0;             
}

static inline uint8_t eth_process_rx(void *state, struct rte_mbuf *mb)
{
  uint64_t sm, dm;
  // struct eth_state *eth_s = (struct eth_state *) state;
  struct eth_pkt *p = (struct eth_pkt *) (mb->buf_addr + mb->data_off);

  if (f_beui16(p->eth.type) == ETH_TYPE_ARP)
  {
    LOG_DEBUG("got an ARP packet");
    return QUEUE_TYPE_ARP_RX;
  }

  memcpy(&sm, &p->eth.src, ETH_ADDR_LEN);
  memcpy(&dm, &p->eth.dst, ETH_ADDR_LEN);
  LOG_DEBUG("rx eth: src_mac=%08x:%05u dst_mac=%08x:%05u", sm, dm);

  return 0;
}

static inline uint8_t eth_process_tx(void *state, struct rte_mbuf *mb)
{
  uint64_t sm, dm;
  // struct eth_state *eth_s = (struct eth_state *) state;
  struct eth_pkt *p = (struct eth_pkt *) (mb->buf_addr + mb->data_off);
  struct eth_addr addr_src, addr_dst;

  mac_from_text("b8:59:9f:c4:af:66", addr_src.addr);
  mac_from_text("b8:59:9f:c4:af:e6", addr_dst.addr);

  p->eth.src = addr_src;
  p->eth.dst = addr_dst;
  p->eth.type = t_beui16(ETH_TYPE_IP);
  memcpy(&sm, &p->eth.src, ETH_ADDR_LEN);
  memcpy(&dm, &p->eth.dst, ETH_ADDR_LEN);
  LOG_DEBUG("tx eth: src_mac=%012" PRIx64 " dst_mac=%012" PRIx64,
          (uint64_t)(be64toh(sm)),
          (uint64_t)(be64toh(dm)));

  return 0;
}

static inline uint8_t eth_process_queues()
{
  return 0;
}

#endif
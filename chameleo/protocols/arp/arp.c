// #include <stddef.h>
// #include <stdio.h>
// #include <stdlib.h>
// #include <string.h>
// #include <unistd.h>
// #include <stdint.h>

// #include "arp.h"
// #include "../../config/config.h"
// #include "../../protocols/eth/eth_pkt.h"
// #include "../../../utils/log/log.h"

// static inline int response_tx(const void *dst_mac, uint32_t dst_ip);
// static inline int request_tx(uint32_t dst_ip);
// static inline struct arp_entry *ae_lookup(struct arp_table *at, uint32_t ip);

// int arp_init(struct arp_table *at, 
//     struct configuration *config, struct rte_ether_addr *eth_addr)
// {
//   struct arp_entry *ae = malloc(sizeof(struct arp_entry));
//   if (ae == NULL)
//   {
//     LOG_ERROR("failed to allocate arp entries");
//     return -1;
//   }

//   ae->status = 0;
//   ae->ip = config->ip;
//   memcpy(ae->mac, eth_addr, ETH_ADDR_LEN);
//   ae->next = NULL;
//   ae->prev = NULL;
//   at->entries = ae;
//   at->len = 0;

//   return 0;
// }

// int arp_request(struct arp_table *at, uint32_t ip, uint64_t *mac)
// {
//   struct arp_entry *ae;

//   *mac = 0;

//   /* found entry */
//   if ((ae = ae_lookup(at, ip)) != NULL) 
//   {
//     if (ae->status == 0) 
//     {
//       memcpy(mac, ae->mac, 6);
//       return 0;
//     } else 
//     {
//       /* request still pending */
//       comp->ptr = mac;
//       comp->el.next = (void *) ae->compl;
//       ae->compl = comp;
//       return 1;
//     }
//   }

//   /* allocate cache entry */
//   if ((ae = malloc(sizeof(*ae))) == NULL) 
//   {
//     LOG_ERROR("malloc failed");
//     return -1;
//   }

//   ae->status = 1;
//   ae->ip = ip;
//   ae->compl = comp;
//   comp->el.next = NULL;
//   comp->ptr = mac;

//   /* send out request */
//   if (request_tx(ip) != 0) 
//   {
//     /* timeout will take care of re-trying */
//     LOG_ERROR("arp timeout sending out request failed");
//   }

//   /* arm timeout */
//   ae->timeout = config.arp_to;
//   util_timeout_arm(&timeout_mgr, &ae->to, ae->timeout, TO_ARP_REQ);

//   /* insert into list */
//   ae->prev = NULL;
//   ae->next = at->entries;
//   if (at->entries != NULL) 
//   {
//     at->entries->prev = ae;
//   }
//   at->entries = ae;

//   return 1;
// }

// void arp_packet(const void *pkt, uint16_t len)
// {
//   const struct pkt_arp *parp = pkt;
//   const struct arp_hdr *arp = &parp->arp;
//   uint16_t op;
//   uint64_t cnt = 1;
//   int fd, ret;
//   struct arp_entry *ae;
//   struct nicif_completion *comp, *comp_next;

//   /* filter out bad packets */
//   if (f_beui16(arp->htype) != ARP_HTYPE_ETHERNET ||
//       f_beui16(arp->ptype) != ARP_PTYPE_IPV4 ||
//       arp->hlen != 6 || arp->plen != 4)
//   {
//     LOG_ERROR("invalid packet received htype=%x ptype=%x hlen=%x plen=%x", 
//         f_beui16(arp->htype), f_beui16(arp->ptype), arp->hlen, arp->plen);
//     return;
//   }
//   op = f_beui16(arp->oper);
//   if (op == ARP_OPER_REQUEST) {
//     /* handle ARP request */
//     if (f_beui32(arp->tpa) != config.ip) 
//     {
//       /* ARP request not for me */
//       return;
//     }

//     /* send response */
//     if (response_tx(&arp->sha, f_beui32(arp->spa)) != 0) 
//     {
//       LOG_ERROR("sending response failed");
//       return;
//     }
//   } else if (op == ARP_OPER_REPLY) 
//   {
//     /* handle ARP response */
//     if ((ae = ae_lookup(f_beui32(arp->spa))) == NULL) 
//     {
//       return;
//     }

//     if (ae->status == 1) 
//     {
//       /* disarm timeout */
//       util_timeout_disarm(&timeout_mgr, &ae->to);
//     }

//     /* fill in information on arp entry */
//     memcpy(ae->mac, &arp->sha, ETH_ADDR_LEN);
//     ae->status = 0;

//     /* notify waiting connections */
//     for (comp = ae->compl; comp != NULL; comp = comp_next) 
//     {
//       comp_next = (void *) comp->el.next;

//       memcpy(comp->ptr, ae->mac, ETH_ADDR_LEN);
//       comp->status = 0;
//       fd = comp->notify_fd;
//       nbqueue_enq(comp->q, &comp->el);
//       if (fd != -1) {
//         ret = write(fd, &cnt, sizeof(cnt));
//         if (ret <= 0) {
//           LOG_ERROR("error writing to notify fd");
//         }
//       }
//     }
//     ae->compl = NULL;
//   }
// }

// void arp_timeout(struct timeout *to, enum timeout_type type)
// {
//   int fd;
//   ssize_t ret;
//   uint64_t cnt = 1;
//   struct nicif_completion *comp, *comp_next;
//   struct arp_entry *ae = (struct arp_entry *)
//     ((uintptr_t) to - offsetof(struct arp_entry, to));

//   ARP_DEBUG("arp_timeout(%x): timeout=%uus\n", ae->ip, ae->timeout);

//   /* the arp entry should not be ready or the timeout would have been
//    * cancelled */
//   if (ae->status == 0) 
//   {
//     LOG_ERROR("arp timeout arp entry marked as ready");
//     abort();
//   }

//   /* if we received the maximum timeout, abort */
//   if (ae->timeout * 2 >= config.arp_to_max) 
//   {

//     /* notify waiting connections */
//     for (comp = ae->compl; comp != NULL; comp = comp_next) 
//     {
//       comp_next = (void *) comp->el.next;

//       comp->status = -1;
//       fd = comp->notify_fd;
//       nbqueue_enq(comp->q, &comp->el);
//       if (fd != -1) 
//       {
//         ret = write(fd, &cnt, sizeof(cnt));
//         if (ret <= 0) 
//         {
//           LOG_ERROR("error writing to notify fd");
//         }
//       }
//     }

//     /* remove arp entry from cache */
//     if (ae->prev != NULL) 
//     {
//       ae->prev->next = ae->next;
//     } else 
//     {
//       arp_table = ae->next;
//     }
    
//     if (ae->next != NULL) 
//     {
//       ae->next->prev = ae->prev;
//     }

//     /* free entry */
//     free(ae);
//     return;
//   }

//   /* send out another request */
//   if (request_tx(ae->ip) != 0) 
//   {
//     LOG_ERROR("arp timeout sending out request failed")
//   }

//   /* rearm timeout */
//   ae->timeout *= 2;
//   util_timeout_arm(&timeout_mgr, &ae->to, ae->timeout, TO_ARP_REQ);
// }

// static inline int response_tx(const void *dst_mac, uint32_t dst_ip)
// {
//   struct pkt_arp *parp_out;
//   uint32_t new_tail;

//   /* allocate tx buffer */
//   if (nicif_tx_alloc(sizeof(*parp_out), (void **) &parp_out, &new_tail) != 0) 
//   {
//     LOG_ERROR("failed to allocate tx buffer");
//     return -1;
//   }

//   /* fill in response */
//   memcpy(&parp_out->eth.src, &eth_addr, ETH_ADDR_LEN);
//   memcpy(&parp_out->arp.sha, &eth_addr, ETH_ADDR_LEN);
//   memcpy(&parp_out->eth.dest, dst_mac, ETH_ADDR_LEN);
//   memcpy(&parp_out->arp.tha, dst_mac, ETH_ADDR_LEN);
//   parp_out->arp.spa = t_beui32(config.ip);
//   parp_out->arp.tpa = t_beui32(dst_ip);

//   parp_out->eth.type = t_beui16(ETH_TYPE_ARP);

//   parp_out->arp.htype = t_beui16(ARP_HTYPE_ETHERNET);
//   parp_out->arp.ptype = t_beui16(ARP_PTYPE_IPV4);
//   parp_out->arp.hlen = 6;
//   parp_out->arp.plen = 4;
//   parp_out->arp.oper = t_beui16(ARP_OPER_REPLY);

//   nicif_tx_send(new_tail, 0);

//   return 0;
// }

// static inline int request_tx(uint32_t dst_ip)
// {
//   struct pkt_arp *parp_out;
//   uint32_t new_tail;
//   uint64_t dst_mac = 0xffffffffffffULL;

//   /* allocate tx buffer */
//   if (nicif_tx_alloc(sizeof(*parp_out), (void **) &parp_out, &new_tail) != 0) 
//   {
//     LOG_ERROR("failed to allocate tx buffer");
//     return -1;
//   }

//   /* fill in response */
//   memcpy(&parp_out->eth.src, &eth_addr, ETH_ADDR_LEN);
//   memcpy(&parp_out->arp.sha, &eth_addr, ETH_ADDR_LEN);
//   memcpy(&parp_out->eth.dest, &dst_mac, ETH_ADDR_LEN);
//   memcpy(&parp_out->arp.tha, &dst_mac, ETH_ADDR_LEN);
//   parp_out->arp.spa = t_beui32(config.ip);
//   parp_out->arp.tpa = t_beui32(dst_ip);

//   parp_out->eth.type = t_beui16(ETH_TYPE_ARP);

//   parp_out->arp.htype = t_beui16(ARP_HTYPE_ETHERNET);
//   parp_out->arp.ptype = t_beui16(ARP_PTYPE_IPV4);
//   parp_out->arp.hlen = 6;
//   parp_out->arp.plen = 4;
//   parp_out->arp.oper = t_beui16(ARP_OPER_REQUEST);

//   nicif_tx_send(new_tail, 0);

//   return 0;
// }

// static inline struct arp_entry * ae_lookup(struct arp_table *at, uint32_t ip)
// {
//   struct arp_entry *ae;

//   for (ae = at->entries; ae != NULL; ae = ae->next) 
//   {
//     if (ae->ip == ip) 
//     {
//       return ae;
//     }
//   }
//   return NULL;
// }

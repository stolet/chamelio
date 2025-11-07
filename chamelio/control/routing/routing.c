// #include <stdlib.h>
// #include <stdio.h>

// #include "routing.h"
// #include "../../config/config.h"

// static inline __u32 prefix_len_mask(__u8 len);
// static inline struct routing_table_entry *resolve(struct routing_table *rt, __u32 ip);

// int routing_init(struct routing_table *rt, struct configuration *config)
// {
//   struct routing_table_entry *entries;
//   struct config_route *cr;
//   size_t i;
//   __u32 mask;

//   /* count number of entries to be added */
//   rt->len = 1;
//   for (cr = config->routes; cr != NULL; rt->len++, cr = cr->next);

//   /* allocate entries */
//   entries = calloc(rt->len, sizeof(struct routing_table_entry));
//   if (entries == NULL)
//   {
//     LOG_ERROR("allocating routing table entries failed");
//     return -1;
//   }
//   rt->entries = entries;

//   /* first fill in network route based on ip and prefix */
//   mask = prefix_len_mask(config->ip_prefix);
//   entries[0].dest_ip = config->ip & mask;
//   entries[0].dest_mask = mask;
//   entries[0].next_hop = 0;

//   /* fill in routing table */
//   for (i = 1, cr = config->routes; cr != NULL; i++, cr = cr->next) 
//   {
//     mask = prefix_len_mask(cr->ip_prefix);
//     if ((mask & cr->ip) != cr->ip) 
//     {
//       LOG_ERROR("mask removes non-0 bits "
//           "(d=%x m=%x n=%x)",
//         cr->ip, mask, cr->next_hop_ip);
//       return -1;
//     }

//     entries[i].dest_ip = cr->ip;
//     entries[i].dest_mask = mask;
//     entries[i].next_hop = cr->next_hop_ip;
//   }

//   return 0;
// }

// int routing_resolve(struct routing_table *rt, struct nicif_completion *comp, __u32 ip, __u64 *mac)
// {
//   struct routing_table_entry *rte;

//   while (1) {
//     rte = resolve(rt, ip);
//     if (rte == NULL) 
//     {
//       LOG_ERROR("routing failed");
//       return -1;
//     }

//     if (rte->next_hop == 0) 
//       break;

//     ip = rte->next_hop;
//   }

//   return  arp_request(comp, ip, mac);
// }

// static inline __u32 prefix_len_mask(__u8 len)
// {
//   return ~((1ULL << (32 - len)) - 1);
// }

// static inline struct routing_table_entry *resolve(struct routing_table *rt, __u32 ip)
// {
//   size_t i;

//   for (i = 0; i < rt->len; i++) 
//   {
//     if (rt->entries[i].dest_ip == (ip & rt->entries[i].dest_mask)) 
//     {
//       return &rt->entries[i];
//     }
//   }

//   return NULL;
// }

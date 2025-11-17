#ifndef ROUTING_H_
#define ROUTING_H_

#include <linux/types.h>
#include <stdlib.h>

struct routing_table {
  size_t len;
  struct routing_table_entry *entries;
};

/** Routing table entry */
struct routing_table_entry {
  /** Destination IP address */
  __u32 dest_ip;
  /** Destination IP address mask */
  __u32 dest_mask;
  /** Next hop IP address */
  __u32 next_hop;
};

#endif
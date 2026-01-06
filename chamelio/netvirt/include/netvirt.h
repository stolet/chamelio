#ifndef NETVIRT_H_
#define NETVIRT_H_

#include <linux/types.h>

/* Must be power of 2 */
#define NETVIRT_LEN 64
#define NETVIRT_INVALID -1

/* Entry in a netvirt table */
struct netvirt_entry {
  /* Key A used to index */
  __u32 keya;
  /* Key B used to index */
  __u32 keyb;
  /* Guest ID */
  __u32 gid;
  /* GRE key */
  __u32 gre_key;
  /* Inner IP header IP address */
  __u32 inner_ip;
  /* Outer IP header IP address */
  __u32 outer_ip;
};

/* Table with network virtualization configuration */
struct netvirt_table {
  /* Values in table indexed by hash */
  struct netvirt_entry vals[NETVIRT_LEN];
};

/* Initializes netvirt table */
void netvirt_table_init(struct netvirt_table *table);
/* Sets entry into table and uses GRE key and inner IP as hash key */
int netvirt_table_set(struct netvirt_table *table, __u32 keya, __u32 keyb,
    __u32 gid, __u32 gre_key, __u32 inner_ip, __u32 outer_ip);
/* Gets entry from table and uses GRE key and inner IP as hash key */
struct netvirt_entry * netvirt_table_get(struct netvirt_table *table,
  __u32 keya, __u32 keyb);

/* Parses network virtualization configuration file */
int netvirt_parser(struct netvirt_table *inner_table, 
    struct netvirt_table *gid_table, const char *config_path);

#endif
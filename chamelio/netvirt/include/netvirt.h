#ifndef NETVIRT_H_
#define NETVIRT_H_

#include <linux/types.h>

/* Must be power of 2 */
#define NETVIRT_LEN 64
#define NETVIRT_INVALID -1

struct ip_table_entry {
  /* GRE key for this virtual network */
  __u32 gre_key;
  /* Inner IP address */
  __u32 inner_ip;
  /* Outer IP used in tunnel */
  __u32 outer_ip;
};

/* Table of outer IP addresses */
struct ip_table {
  /* Number of elements in hash table */
  __u32 len;
  /* Array of outer IP addresses indexed by hash of GRE key and inner IP */
  struct ip_table_entry ips[NETVIRT_LEN];
};

struct gre_table_entry {
  /* Outer IP used in tunnel */
  __u32 outer_ip;
  /* Guest ID assigned by Chamelio */
  __u32 gid;
  /* GRE key for this virtual network */
  __u32 gre_key;
};

/* Table of GRE keys */
struct gre_table {
  /* NUmber of elements in hash table */
  __u32 len;
  /* Array of GRE keys indexed by hash of outer IP and guest ID */
  struct gre_table_entry gre[NETVIRT_LEN];
};

/* Initializes the IP table with invalid IDs */
void netvirt_ip_init(struct ip_table *table);
/* Hashes gre_key and inner_ip and adds outer_ip to the table */
int netvirt_ip_set(struct ip_table *table, __u32 gre_key, __u32 inner_ip, __u32 outer_ip);
/* Hashes gre_key and inner_ip and returns outer_ip from the table */
struct ip_table_entry * netvirt_ip_get(struct ip_table *table,
    __u32 gre_key, __u32 inner_ip);
/* Initializes the GRE table with invalid IDs */
void netvirt_gre_init(struct gre_table *table);
/* Hashes outer_ip and gid and adds gre_key to the table */
int netvirt_gre_set(struct gre_table *table, __u32 outer_ip, __u8 gid, __u32 gre_key);
/* Hashes outer_ip and gid and returns gre key from the table */
struct gre_table_entry * netvirt_gre_get(struct gre_table *table,
    __u32 outer_ip, __u8 gid);
/* Parses CSV configuration file and populates ip_table and gre_table */
int netvirt_parser(struct ip_table *ip_tbl, struct gre_table *gre_tbl,
    const char *config_path);

#endif
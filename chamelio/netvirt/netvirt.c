#include <stdlib.h>
#include <linux/types.h>

#include "netvirt.h"

static inline __u64 hash2(__u32 a, __u32 b);

void netvirt_ip_init(struct ip_table *table)
{
  int i;
  struct ip_table_entry *e;

  for (i = 0; i < NETVIRT_LEN; i++)
  {
    e = &table->ips[i];
    e->gre_key = NETVIRT_INVALID;
    e->inner_ip = NETVIRT_INVALID;
    e->outer_ip = NETVIRT_INVALID;
  }
}

int netvirt_ip_set(struct ip_table *table, 
    __u32 gre_key, __u32 inner_ip, __u32 outer_ip)
{
  int i;
  __u32 hash;
  struct ip_table_entry *e;

  hash = hash2(gre_key, inner_ip);
  for (i = 0; i < NETVIRT_LEN; i++)
  {
    e = &table->ips[hash];

    if (e->outer_ip == NETVIRT_INVALID || 
        (e->gre_key == gre_key && e->inner_ip == inner_ip))
    {
      e->gre_key = gre_key;
      e->inner_ip = inner_ip;
      e->outer_ip = outer_ip;
      return 0;
    }

    hash += 1;
    hash &= NETVIRT_LEN - 1;
  }

  return -1;
}

struct ip_table_entry * netvirt_ip_get(struct ip_table *table,
    __u32 gre_key, __u32 inner_ip)
{
  int i;
  __u32 hash;
  struct ip_table_entry *e;

  hash = hash2(gre_key, inner_ip);
  for (i = 0; i < NETVIRT_LEN; i++)
  {
    e = &table->ips[hash];
    if (e->gre_key == gre_key && e->inner_ip == inner_ip)
      return e;
    else if (e->outer_ip == NETVIRT_INVALID)
      return NULL;

    hash += 1;
    hash &= NETVIRT_LEN - 1;
  }

  return NULL;
}

void netvirt_gre_init(struct gre_table *table)
{
  int i;
  struct gre_table_entry *e;

  for (i = 0; i < NETVIRT_LEN; i++)
  {
    e = &table->gre[i];
    e->gid = NETVIRT_INVALID;
    e->gre_key = NETVIRT_INVALID;
    e->outer_ip = NETVIRT_INVALID;
  }
}

int netvirt_gre_set(struct gre_table *table, 
    __u32 outer_ip, __u8 gid, __u32 gre_key)
{
  int i;
  __u32 hash;
  struct gre_table_entry *e;

  hash = hash2(outer_ip, gid);
  for (i = 0; i < NETVIRT_LEN; i++)
  {
    e = &table->gre[hash];
    if (e->gre_key == NETVIRT_INVALID || 
        (e->outer_ip == outer_ip && e->gid == gid))
    {
      e->outer_ip = outer_ip;
      e->gid = gid;
      e->gre_key = gre_key;
      return 0;
    }

    hash += 1;
    hash &= NETVIRT_LEN - 1;
  }

  return -1;
}

struct gre_table_entry * netvirt_gre_get(struct gre_table *table,
    __u32 outer_ip, __u8 gid)
{
  int i;
  __u32 hash;
  struct gre_table_entry *e;

  hash = hash2(outer_ip, gid);
  for (i = 0; i < NETVIRT_LEN; i ++)
  {
    e = &table->gre[hash];
    if (e->outer_ip == outer_ip && e->gid == gid)
      return e;
    else if (e->gre_key == NETVIRT_INVALID)
      return NULL;

    hash += 1;
    hash &= NETVIRT_LEN - 1;
  }

  return NULL;
}

static inline __u64 hash2(__u32 a, __u32 b)
{
  /* FNV offset basis ensures that if we hash an empty inset we dont's get zero */
  __u64 h = 0xcbf29ce484222325ULL;
  
  /* Mix a with FNV prime to scramble and spread bits */
  h ^= a;
  h *= 0x100000001b3ULL;
  
  /* Mix b with FNV prime to scramble and spread bits */
  h ^= b;
  h *= 0x100000001b3ULL;
  
  return h & (NETVIRT_LEN - 1);
}
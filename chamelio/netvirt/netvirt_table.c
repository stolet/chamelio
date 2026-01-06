#include <stdlib.h>
#include <linux/types.h>

#include "log.h"
#include "netvirt.h"

static inline __u64 hash_ints(__u32 a, __u32 b);

void netvirt_table_init(struct netvirt_table *table)
{
  int i;
  struct netvirt_entry *e;

  for (i = 0; i < NETVIRT_LEN; i++)
  {
    e = &table->vals[i];
    e->keya = NETVIRT_INVALID;
    e->keyb = NETVIRT_INVALID;
    e->gid = NETVIRT_INVALID;
    e->gre_key = NETVIRT_INVALID;
    e->inner_ip = NETVIRT_INVALID;
    e->outer_ip = NETVIRT_INVALID;
  }
}

int netvirt_table_set(struct netvirt_table *table, __u32 keya, __u32 keyb,
    __u32 gid, __u32 gre_key, __u32 inner_ip, __u32 outer_ip)
{
  int i;
  __u32 hash;
  struct netvirt_entry *e;

  hash = hash_ints(keya, keyb);
  for (i = 0; i < NETVIRT_LEN; i++)
  {
    e = &table->vals[hash];
    if (e->keya == NETVIRT_INVALID || (e->keya == keya && e->keyb == keyb))
    {
      e->keya = keya;
      e->keyb = keyb;
      e->gid = gid;
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

struct netvirt_entry * netvirt_table_get(struct netvirt_table *table,
    __u32 keya, __u32 keyb)
{
  int i;
  __u32 hash;
  struct netvirt_entry *e;

  hash = hash_ints(keya, keyb);
  for (i = 0; i < NETVIRT_LEN; i++)
  {
    e = &table->vals[hash];
    if (e->keya == keya && e->keyb == keyb)
      return e;
    else if (e->keya == NETVIRT_INVALID)
      return NULL;

    hash += 1;
    hash &= NETVIRT_LEN - 1;
  }

  return NULL;
}

static inline __u64 hash_ints(__u32 a, __u32 b)
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
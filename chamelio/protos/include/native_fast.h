#ifndef NATIVE_FAST_H_
#define NATIVE_FAST_H_

#include <string.h>
#include <linux/types.h>

#include <rte_ip4.h>

#include "queue_fns.h"
#include "scheduler_fns.h"
#include "clock.h"
#include "utils_sync.h"

#ifndef __always_inline
#define __always_inline inline __attribute__((always_inline))
#endif

#define SEC(name)

static __always_inline void *ebpf_memcpy(void *dst, void *src, size_t len)
{
  return memcpy(dst, src, len);
}

static __always_inline void ebpf_print(int a)
{
  (void) a;
}

static __always_inline __u16 ebpf_ipv4_checksum(void *ip_hdr)
{
  return rte_ipv4_cksum(ip_hdr);
}

static __always_inline __u16 ebpf_ipv4_udptcp_cksum(void *ip_hdr, void *l4_hdr)
{
  return rte_ipv4_udptcp_cksum(ip_hdr, l4_hdr);
}

static __always_inline __u64 ebpf_rdtsc(void)
{
  return clock_rdtsc();
}

static __always_inline __u64 ebpf_now_us(void)
{
  return clock_tsc_to_us(clock_rdtsc());
}

static __always_inline __u64 ebpf_rate_delay_tsc(__u32 bytes, __u32 rate_kbps)
{
  __u64 cycles_per_us;
  __u64 nr;

  if (bytes == 0 || rate_kbps == 0)
    return 0;

  cycles_per_us = clock_us_to_tsc(1);
  if (cycles_per_us == 0)
    return 0;

  nr = (__u64) bytes * 8 * 1000 * cycles_per_us;
  return (nr + rate_kbps - 1) / rate_kbps;
}

static __always_inline void *ebpf_map_get(void *map_base, __u32 len)
{
  (void) len;
  return map_base;
}

static __always_inline void *ebpf_map_lookup(void *map_base, __u64 id,
    __u64 elsize)
{
  return (__u8 *) map_base + (id * elsize);
}

static __always_inline struct cham_sched_entry *ebpf_sched_head(
    struct cham_scheduler *sched, __u64 elsize)
{
  (void) elsize;
  return sched_head(sched);
}

static __always_inline struct cham_sched_entry *cham_native_sched_head(
    struct cham_scheduler *sched, __u64 elsize)
{
  (void) elsize;
  return sched_head(sched);
}

static __always_inline void *ebpf_queue_tail(struct equeue *q, __u64 elsize)
{
  (void) elsize;
  return queue_tail(q);
}

static __always_inline void *ebpf_queue_head(struct dqueue *q, __u64 elsize)
{
  (void) elsize;
  return queue_head(q);
}

static __always_inline void ebpf_spin_lock(volatile __u32 *sl)
{
  util_spin_lock(sl);
}

static __always_inline void ebpf_spin_unlock(volatile __u32 *sl)
{
  util_spin_unlock(sl);
}

#endif

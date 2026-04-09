#ifndef UTILS_SYNC_H_
#define UTILS_SYNC_H_

#include <linux/types.h>

static inline void util_spin_lock(volatile __u32 *sl)
{
  __u32 lock_val = 1;

  asm volatile (
      "1:\n"
      "xchg %[locked], %[lv]\n"
      "test %[lv], %[lv]\n"
      "jz 3f\n"
      "2:\n"
      "pause\n"
      "cmpl $0, %[locked]\n"
      "jnz 2b\n"
      "jmp 1b\n"
      "3:\n"
      : [locked] "=m" (*sl), [lv] "=q" (lock_val)
      : "[lv]" (lock_val)
      : "memory");
}

static inline void util_spin_unlock(volatile __u32 *sl)
{
  __u32 unlock_val = 0;

  asm volatile (
      "xchg %[locked], %[ulv]\n"
      : [locked] "=m" (*sl), [ulv] "=q" (unlock_val)
      : "[ulv]" (unlock_val)
      : "memory");
}

#endif
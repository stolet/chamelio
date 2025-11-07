#ifndef UTILS_H_
#define UTILS_H_

#include <linux/types.h>

#define MEM_BARRIER() __asm__ volatile("" ::: "memory")
#define STATIC_ASSERT(COND,MSG) typedef char static_assertion_##MSG[(COND)?1:-1]

struct beui16 { __u16 x; } __attribute__((packed));
struct beui32 { __u32 x; } __attribute__((packed));
struct beui64 { __u64 x; } __attribute__((packed));
typedef struct beui16 beui16_t;
typedef struct beui32 beui32_t;
typedef struct beui64 beui64_t;

static inline __u16 f_beui16(beui16_t x) { return __builtin_bswap16(x.x); }
static inline __u32 f_beui32(beui32_t x) { return __builtin_bswap32(x.x); }
static inline __u64 f_beui64(beui64_t x) { return __builtin_bswap64(x.x); }

static inline beui16_t t_beui16(__u16 x)
{
  beui16_t b;
  b.x = __builtin_bswap16(x);
  return b;
}

static inline beui32_t t_beui32(__u32 x)
{
  beui32_t b;
  b.x = __builtin_bswap32(x);
  return b;
}

static inline beui64_t t_beui64(__u64 x)
{
  beui64_t b;
  b.x = __builtin_bswap64(x);
  return b;
}

#endif
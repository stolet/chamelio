#ifndef UTILS_H_
#define UTILS_H_

#include <stdint.h>

struct beui16 { uint16_t x; } __attribute__((packed));
struct beui32 { uint32_t x; } __attribute__((packed));
struct beui64 { uint64_t x; } __attribute__((packed));
typedef struct beui16 beui16_t;
typedef struct beui32 beui32_t;
typedef struct beui64 beui64_t;

static inline uint16_t f_beui16(beui16_t x) { return __builtin_bswap16(x.x); }
static inline uint32_t f_beui32(beui32_t x) { return __builtin_bswap32(x.x); }
static inline uint64_t f_beui64(beui64_t x) { return __builtin_bswap64(x.x); }

static inline beui16_t t_beui16(uint16_t x)
{
  beui16_t b;
  b.x = __builtin_bswap16(x);
  return b;
}

static inline beui32_t t_beui32(uint32_t x)
{
  beui32_t b;
  b.x = __builtin_bswap32(x);
  return b;
}

static inline beui64_t t_beui64(uint64_t x)
{
  beui64_t b;
  b.x = __builtin_bswap64(x);
  return b;
}

#endif
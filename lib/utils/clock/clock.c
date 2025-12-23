#include <time.h>
#include <linux/types.h>
#include <unistd.h>

static double tsc_per_us = 0.0;

__u64 clock_rdtsc(void)
{
  __u32 eax, edx;
  asm volatile ("rdtsc" : "=a" (eax), "=d" (edx));
  return ((__u64)edx << 32) | eax;
}

int clock_calibrate_tsc(void)
{
  if (tsc_per_us != 0.0) 
    return -1;

  struct timespec ts_before, ts_after;
  if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts_before) != 0)
    return -1;
  
  __u64 t0 = clock_rdtsc();
  usleep(10000);
  __u64 t1 = clock_rdtsc();

  if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts_after) != 0)
    return -1;

  __u64 dt_cycles = t1 - t0;
  __u64 us_before = (__u64)ts_before.tv_sec * 1000000ULL 
      + (__u64)(ts_before.tv_nsec / 1000ULL);
  __u64 us_after  = (__u64)ts_after.tv_sec  * 1000000ULL 
      + (__u64)(ts_after.tv_nsec  / 1000ULL);
  __u64 dt_us = us_after - us_before;
  
  if (dt_us == 0) 
    dt_us = 1;

  tsc_per_us = (double)dt_cycles / (double)dt_us;
  if (tsc_per_us <= 0.0)
    return -1;
    
  return 0;
}

__u64 clock_us_since_tsc(__u64 tsc_then)
{
  __u64 tsc_now = clock_rdtsc();
  double delta = (double)(tsc_now - tsc_then);
  if (delta < 0.0) delta = 0.0;
  double us = delta / tsc_per_us;
  if (us < 0.0) us = 0.0;
  return (__u64)(us + 0.5);
}

__u64 clock_tsc_after_us(__u64 us)
{
  __u64 tsc_now = clock_rdtsc();
  return tsc_now + (us * tsc_per_us);
}

__u64 clock_us_to_tsc(__u64 us)
{
  return us * tsc_per_us;
}

__u64 clock_now_ns(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return (__u64)ts.tv_sec * 1000000000ULL + (__u64)ts.tv_nsec;
}
#ifndef CLOCK_H_
#define CLOCK_H_

#include <linux/types.h>

/* Reads the CPU timestamp counter */
__u64 clock_rdtsc(void);
/* Calibrates tsc readings to real time */
int clock_calibrate_tsc(void);
/* Microseconds elapsed since tsc reading  */
__u64 clock_us_since_tsc(__u64 tsc_then);
/* TSC value after given number of microseconds */
__u64 clock_tsc_after_us(__u64 us);
/* Current time in nanoseconds */
__u64 clock_now_ns(void);

#endif
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
/* Converts microseconds to cycles */
__u64 clock_us_to_tsc(__u64 us);
/* Converts a TSC reading to microseconds */
__u64 clock_tsc_to_us(__u64 tsc);
/* Current time in nanoseconds */
__u64 clock_now_ns(void);

/* Publish a wakeup using TAS inter-publication timestamp suppression. */
void clock_notify(int fd, __u64 *last_ns, __u64 grace_ns);
/* Require an idle grace interval and one additional poll before sleeping. */
int clock_canblock(__u64 *last_ns, int *extra_poll, int busy,
    __u64 grace_ns);
/* Convert an absolute TSC deadline into a rounded-down epoll timeout. */
int clock_deadline_ms(__u64 deadline);

#endif

/* UDP client that sends fixed-size messages, each stamped with
 * (seq, rdtsc) and a magic. Matches replies by seq to compute RTTs.
 *
 * Reports per-second throughput (MB/s and packets/s) and cumulative
 * latency percentiles (p50, p99, p99.9) in microseconds.
 *
 * Usage:
 *   ./udp_linux_client <server_ip> <port> [--msg-size N] [--duration S]
 *
 * Notes:
 *   - --msg-size is the TOTAL UDP payload bytes (header included).
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <linux/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

struct payload_hdr {
  __u64 tsc;
}__attribute__((packed));

/* Default arg values */
size_t msize = 64; // Total payload bytes: includes payload_hdr
int duration = 30;
int max_pending = 1;
const char *server_ip;
int port;

/* Variables used to report stats */
__u64 t_start_ns, t_end_ns;
__u64 t_last_report_ns, t_next_report_ns;
__u64 tx_bytes_interval, tx_pkts_interval;
__u64 total_sent_pkts;

/* Cycles per microsecond */
static double tsc_per_us = 0.0;

/* RTT histogram (µs buckets) */
/* Clamp histogram at 1 second */
#define MAX_RTT_US 1000000u  
static __u64 *rtt_hist = NULL;
static __u64 total_rx = 0;

static inline __u64 util_rdtsc(void)
{
  __u32 eax, edx;
  asm volatile ("rdtsc" : "=a" (eax), "=d" (edx));
  return ((__u64)edx << 32) | eax;
}

static inline void calibrate_tsc(void)
{
  if (tsc_per_us != 0.0) return;

  struct timespec ts_before, ts_after;
  if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts_before) != 0)
  {
    perror("calibrate_tsc: clock_gettime before");
    exit(EXIT_FAILURE);
  }

  __u64 t0 = util_rdtsc();
  usleep(10000);
  __u64 t1 = util_rdtsc();

  if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts_after) != 0)
  {
    perror("calibrate_tsc: clock_gettime after");
    exit(EXIT_FAILURE);
  }

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
  {
    fprintf(stderr, "calibrate_tsc: invalid tsc_per_us: %f\n", tsc_per_us);
    exit(EXIT_FAILURE);
  }
}

static inline __u64 us_since_tsc(__u64 tsc_then)
{
  __u64 tsc_now = util_rdtsc();
  double delta = (double)(tsc_now - tsc_then);
  if (delta < 0.0) delta = 0.0;
  double us = delta / tsc_per_us;
  if (us < 0.0) us = 0.0;
  return (__u64)(us + 0.5);
}

static inline __u64 now_ns(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return (__u64)ts.tv_sec * 1000000000ULL + (__u64)ts.tv_nsec;
}

static inline void hist_add(__u64 rtt_us)
{
  if (rtt_us > MAX_RTT_US) rtt_us = MAX_RTT_US;
  rtt_hist[rtt_us]++;
  total_rx++;
}

static bool hist_percentiles(__u64 *p50, __u64 *p99, __u64 *p999)
{
  if (total_rx == 0) return false;

  __u64 N = total_rx;
  __u64 t50  = (N * 50  + 100 - 1) / 100;
  __u64 t99  = (N * 99  + 100 - 1) / 100;
  __u64 t999 = (N * 999 + 1000 - 1) / 1000;

  __u64 acc = 0;
  __u64 v50 = 0, v99 = 0, v999 = 0;
  bool got50 = false, got99 = false, got999 = false;

  for (__u32 i = 0; i <= MAX_RTT_US; i++)
  {
    __u64 c = rtt_hist[i];
    
    if (c == 0) 
      continue;
    
    acc += c;
    
    if (!got50 && acc >= t50)
    {
      v50 = i; 
      got50 = true;
    }
      
    if (!got99 && acc >= t99)
    {
      v99 = i; 
      got99 = true;
    }

    if (!got999 && acc >= t999)
    {
      v999 = i; 
      got999 = true;
    }

    if (got50 && got99 && got999) 
      break;
  }

  *p50 = v50; *p99 = v99; *p999 = v999;
  return true;
}

static void set_nonblock(int fd)
{
  int fl = fcntl(fd, F_GETFL, 0);
  if (fl < 0)
  {
    perror("fcntl(F_GETFL)");
    exit(EXIT_FAILURE);
  }
  
  if (fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) 
  {
    perror("fcntl(F_SETFL O_NONBLOCK)");
    exit(EXIT_FAILURE);
  }
}

static int parse_args(int argc, char **argv)
{
  if (argc < 3)
  {
    fprintf(stderr,
      "Usage: %s <server_ip> <port> [--msg-size N] [--max-peding N] [--duration S]\n",
      argv[0]);
    return -1;
  }

  server_ip = argv[1];
  port = atoi(argv[2]);
  if (port <= 0 || port > 65535)
  {
    fprintf(stderr, "Invalid port\n");
    return -1;
  }

  for (int i = 3; i < argc; i++)
  {
    if ((strcmp(argv[i], "--msg-size") == 0 || 
        strcmp(argv[i], "--size") == 0) && 
        i + 1 < argc)
    {
      msize = (size_t)strtoul(argv[++i], NULL, 10);
    }
    else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc)
    {
      duration = atoi(argv[++i]);
    }
    else if (strcmp(argv[i], "--max-pending") == 0 && i + 1 < argc)
    {
      max_pending = atoi(argv[++i]);
    }
    else
    {
      fprintf(stderr, "Unknown arg: %s\n", argv[i]);
      return -1;
    }
  }

  if (msize < sizeof(struct payload_hdr))
  {
    fprintf(stderr, "msg-size must be at least %zu bytes (header size)\n", 
        sizeof(struct payload_hdr));
    return -1;
  }
  
  return 0;
}

static int init_hist()
{
  rtt_hist = (__u64 *) calloc((size_t) MAX_RTT_US + 1, sizeof(__u64));
  if (!rtt_hist) 
  {
    perror("calloc rtt_hist");
    return -1;
  }
  
  return 0;
}

static void print_stats(__u64 now)
{
  double interval_s, pps, MBps;
  bool have;
  
  __u64 p50 = 0, p99 = 0, p999 = 0;
  
  interval_s = (double)(t_next_report_ns - t_last_report_ns) / 1e9;
  if (interval_s <= 0.0) interval_s = 1.0;

  pps  = (double)tx_pkts_interval / interval_s;
  MBps = ((double)tx_bytes_interval / 1e6) / interval_s;

  have = hist_percentiles(&p50, &p99, &p999);

  printf("[ %3lus ] pps=%10.2f | %8.2f MB/s | rtt_samples=%llu | ",
          (unsigned long)((t_next_report_ns - t_start_ns) / 1000000000ULL),
          pps, MBps, (unsigned long long)total_rx);
  if (have)
  {
    printf("p50=%llu us  p99=%llu us  p99.9=%llu us\n",
            (unsigned long long)p50,
            (unsigned long long)p99,
            (unsigned long long)p999);
  }
  else
  {
    printf("p50=NA  p99=NA  p99.9=NA\n");
  }
  fflush(stdout);

  tx_bytes_interval = 0;
  tx_pkts_interval  = 0;
  t_last_report_ns  = t_next_report_ns;
  t_next_report_ns += 1000000000ULL;

  if (now > t_next_report_ns + 5000000000ULL)
  {
    t_next_report_ns = now + 1000000000ULL;
    t_last_report_ns = now;
  }
}

int main(int argc, char **argv)
{
  int ret, burst;
  ssize_t s;
  struct sockaddr_in dst;
  __u8 *txbuf, *rxbuf;
  __u64 now, rtt_us;
  struct payload_hdr *ph;
  
  __u64 seq = 1;
  socklen_t dstlen = sizeof(dst);
  
  tx_bytes_interval = 0;
  tx_pkts_interval  = 0;
  total_sent_pkts   = 0;
  t_start_ns = now_ns();
  t_last_report_ns = t_start_ns;
  t_next_report_ns = t_start_ns + 1000000000ULL;
  t_end_ns = t_start_ns + (__u64) duration * 1000000000ULL;
  
  ret = parse_args(argc, argv);
  if (ret != 0)
  {
    fprintf(stderr, "failed to parse args\n");
    return EXIT_FAILURE;
  }
  
  ret = init_hist();
  if (ret != 0)
  {
    fprintf(stderr, "failed to init hist\n");
    return EXIT_FAILURE;
  }

  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0)
  {
    perror("socket");
    exit(EXIT_FAILURE);
  }
  set_nonblock(fd);

  memset(&dst, 0, sizeof(dst));
  dst.sin_family = AF_INET;
  dst.sin_port   = htons((__u16)port);
  if (inet_pton(AF_INET, server_ip, &dst.sin_addr) != 1)
  {
    fprintf(stderr, "Invalid server_ip\n");
    return EXIT_FAILURE;
  }

  txbuf = (__u8 *)malloc(msize);
  rxbuf = (__u8 *)malloc(msize);
  if (!txbuf || !rxbuf) 
  {
    perror("malloc buffers");
    exit(EXIT_FAILURE);
  }
  memset(txbuf, 0, msize);
  memset(rxbuf, 0, msize);

  /* We need to calibrate so we know how the 
   * TSC frequency maps to real time 
   */
  calibrate_tsc();

  burst = 0;
  while (true)
  {
    now = now_ns();
    if (duration > 0 && now >= t_end_ns) 
      break;

    /* Send bursts */
    for (; burst < max_pending; burst++)
    {
      ph = (struct payload_hdr *) txbuf;
      ph->tsc = util_rdtsc();

      s = sendto(fd, txbuf, msize, 0, (struct sockaddr *) &dst, dstlen);
      if (s < 0)
      {
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        break;
      }
      
      tx_bytes_interval += (__u64)s;
      tx_pkts_interval  += 1;
      total_sent_pkts   += 1;
      seq++;
    }
      
    /* Drain replies */
    while(1)
    {
      ssize_t r = recvfrom(fd, rxbuf, msize, 0, NULL, NULL);
      if (r < 0)
      {
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;

        perror("recvfrom");
        exit(EXIT_FAILURE);
      }
      
      if ((size_t) r < sizeof(struct payload_hdr))
        continue;
      
      ph = (struct payload_hdr *)rxbuf;
      rtt_us = us_since_tsc(ph->tsc);
      hist_add(rtt_us);
      burst--;
      assert(burst >= 0);
    }

    /* Once per elapsed second */
    now = now_ns();
    if (now >= t_next_report_ns)
      print_stats(now);
      
  }

  /* Final summary */
  __u64 p50 = 0, p99 = 0, p999 = 0;
  bool have = hist_percentiles(&p50, &p99, &p999);
  printf("\n=== Summary ===\n");
  printf("Sent packets: %llu\n", (unsigned long long) total_sent_pkts);
  printf("RTT samples : %llu\n", (unsigned long long) total_rx);
  if (have)
  {
    printf("RTT percentiles (us): p50=%llu  p99=%llu  p99.9=%llu\n",
           (unsigned long long) p50,
           (unsigned long long) p99,
           (unsigned long long) p999);
  }
  else
  {
    printf("No RTT samples collected.\n");
  }

  close(fd);
  return 0;
}

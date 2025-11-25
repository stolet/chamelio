/* Reports per-second throughput (Mbits/s and packets/s) and cumulative
 * latency percentiles (p50, p99, p99.9) in microseconds.
 *
 * Usage:
 *   ./udp_cham_client <server_ip> <port>
 *     [--msg-size N] [--duration S] [--rate R] [--ncores N]
 *
 * Notes:
 *   --msg-size TOTAL UDP payload bytes.
 *   --duration Duration in seconds to run benchmark
 *   --rate Per-core send rate in megabits per second
 *   --ncores Number of cores to use
 */

#define _GNU_SOURCE
#include <udp_lib.h>
#include <errno.h>
#include <stdlib.h>
#include <assert.h>
#include <arpa/inet.h>
#include <string.h>
#include <sys/socket.h>
#include <stdio.h>
#include <time.h>
#include <stdbool.h>
#include <unistd.h>
#include <pthread.h>

struct payload_hdr {
  __u64 tsc;
}__attribute__((packed));

/* Per-core context */
struct core_ctx {
  int id;
  int fd;
  __u8 *txbuf;
  __u8 *rxbuf;
  struct sockaddr_in dst;
  socklen_t dstlen;
  __u64 txb_load_interval;
  __u64 txp_load_interval;
  __u64 txb_tp_interval;
  __u64 txp_tp_interval;
  __u64 total_tx_pkts;
  __u64 total_rx_pkts;
  double rate;
  double tokens;
  __u64 last_tsc;
};


/* Default arg values */
size_t msg_size = 64;
int duration = 30;
double rate = 10.0;
int ncores = 1;
const char *server_ip;
int port;

__u64 t_start_ns, t_end_ns;
__u64 t_last_report_ns, t_next_report_ns;
__u64 txb_load_interval_total, txp_load_interval_total;
__u64 txb_tp_interval_total, txp_tp_interval_total;

/* Cycles per microsecond */
static double tsc_per_us = 0.0;

/* Total received samples across all cores */
static __u64 total_rx = 0;
static __u64 *rtt_hist = NULL;

/* Clamp histogram at 1 second */
#define MAX_RTT_US 1000000u

static pthread_barrier_t start_barrier;
static pthread_t *threads = NULL;
static struct core_ctx *cores = NULL;


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
  __u64 us_before = (__u64) ts_before.tv_sec * 1000000ULL
      + (__u64) (ts_before.tv_nsec / 1000ULL);
  __u64 us_after  = (__u64) ts_after.tv_sec  * 1000000ULL
      + (__u64) (ts_after.tv_nsec  / 1000ULL);
  __u64 dt_us = us_after - us_before;

  if (dt_us == 0)
    dt_us = 1;

  tsc_per_us = (double) dt_cycles / (double) dt_us;
  if (tsc_per_us <= 0.0)
  {
    fprintf(stderr, "calibrate_tsc: invalid tsc_per_us: %f\n", tsc_per_us);
    exit(EXIT_FAILURE);
  }
}

static inline __u64 us_since_tsc(__u64 tsc_then)
{
  __u64 tsc_now = util_rdtsc();
  double delta = (double) (tsc_now - tsc_then);
  
  if (delta < 0.0) 
    delta = 0.0;
  double us = delta / tsc_per_us;
  
  if (us < 0.0) 
    us = 0.0;
  
  return (__u64) (us + 0.5);
}

static inline __u64 now_ns(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return (__u64) ts.tv_sec * 1000000000ULL + (__u64) ts.tv_nsec;
}

static inline void hist_add(__u64 rtt_us)
{
  if (rtt_us > MAX_RTT_US) 
    rtt_us = MAX_RTT_US;

  __atomic_fetch_add(&rtt_hist[rtt_us], 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&total_rx, 1, __ATOMIC_RELAXED);
}

static bool hist_percentiles(__u64 *p50, __u64 *p99, __u64 *p999)
{
  __u64 samples = __atomic_load_n(&total_rx, __ATOMIC_RELAXED);
  if (samples == 0) 
    return false;

  __u64 N = samples;
  __u64 t50  = (N * 50  + 100 - 1) / 100;
  __u64 t99  = (N * 99  + 100 - 1) / 100;
  __u64 t999 = (N * 999 + 1000 - 1) / 1000;

  __u64 acc = 0;
  __u64 v50 = 0, v99 = 0, v999 = 0;
  bool got50 = false, got99 = false, got999 = false;

  for (__u32 i = 0; i <= MAX_RTT_US; i++)
  {
    __u64 c = __atomic_load_n(&rtt_hist[i], __ATOMIC_RELAXED);
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

static int parse_args(int argc, char **argv)
{
  if (argc < 3)
  {
    fprintf(stderr,
        "Usage: %s <server_ip> <port> [--msg-size N]"
        " [--duration S] [--rate R] [--ncores N]\n",
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
      msg_size = (size_t)strtoul(argv[++i], NULL, 10);
    }
    else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc)
    {
      duration = atoi(argv[++i]);
    }
    else if (strcmp(argv[i], "--rate") == 0 && i + 1 < argc)
    {
      rate = strtod(argv[++i], NULL);
    }
    else if ((strcmp(argv[i], "--ncores") == 0) && i + 1 < argc)
    {
      ncores = atoi(argv[++i]);
    }
    else
    {
      fprintf(stderr, "Unknown arg: %s\n", argv[i]);
      return -1;
    }
  }

  if (rate <= 0.0)
  {
    fprintf(stderr, "rate must be > 0\n");
    return -1;
  }

  if (msg_size < sizeof(struct payload_hdr))
  {
    fprintf(stderr, "msg-size must be at least %zu bytes (header size)\n",
        sizeof(struct payload_hdr));
    return -1;
  }

  if (ncores <= 0)
    ncores = 1;

  return 0;
}

static int init_hist(void)
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
  double interval_s, rps_load, mbps_load, rps_tp, mbps_tp;
  bool have;

  __u64 p50 = 0, p99 = 0, p999 = 0;

  interval_s = (double) (t_next_report_ns - t_last_report_ns) / 1e9;
  if (interval_s <= 0.0) interval_s = 1.0;

  /* Aggregate per-core and reset per-core intervals */
  __u64 bytes_load = 0, pkts_load = 0, bytes_tp = 0, pkts_tp = 0;
  for (int i = 0; i < ncores; i++)
  {
    bytes_load += cores[i].txb_load_interval;
    pkts_load  += cores[i].txp_load_interval;
    bytes_tp += cores[i].txb_tp_interval;
    pkts_tp  += cores[i].txp_tp_interval;
    cores[i].txb_load_interval = 0;
    cores[i].txp_load_interval  = 0;
    cores[i].txb_tp_interval = 0;
    cores[i].txp_tp_interval  = 0;
  }
  txb_load_interval_total = bytes_load;
  txp_load_interval_total  = pkts_load;
  txb_tp_interval_total = bytes_tp;
  txp_tp_interval_total  = pkts_tp;

  rps_load  = (double) txp_load_interval_total / interval_s;
  mbps_load = ((double) txb_load_interval_total * 8.0 / 1e6) / interval_s;
  rps_tp  = (double) txp_tp_interval_total / interval_s;
  mbps_tp = ((double) txb_tp_interval_total * 8.0 / 1e6) / interval_s;

  have = hist_percentiles(&p50, &p99, &p999);

  printf("[ %3lus ] load=%lld rps | load=%8.2f Mb/s | tp=%lld rps | tp=%8.2f Mb/s | ",
          (unsigned long)((t_next_report_ns - t_start_ns) / 1000000000ULL),
          (__u64) rps_load, mbps_load, (__u64) rps_tp, mbps_tp);
  if (have)
  {
    printf("p50=%llu us  p99=%llu us  p99.9=%llu us\n",
            (unsigned long long) p50,
            (unsigned long long) p99,
            (unsigned long long) p999);
  }
  else
  {
    printf("p50=NA  p99=NA  p99.9=NA\n");
  }
  fflush(stdout);

  t_last_report_ns  = t_next_report_ns;
  t_next_report_ns += 1000000000ULL;

  if (now > t_next_report_ns + 5000000000ULL)
  {
    t_next_report_ns = now + 1000000000ULL;
    t_last_report_ns = now;
  }
}

static void *core_thread(void *arg)
{
  struct core_ctx *c = (struct core_ctx *) arg;
  int r, s;
  __u64 start_tsc, elapsed, rtt_us;
  struct payload_hdr *ph;

  if (c->id != 0)
    sleep(1);

  /* Each core has its own UDP context and socket */
  int ret = udp_ctx_new();
  if (ret != 0)
  {
    fprintf(stderr, "core %d: failed to create UDP context\n", c->id);
    pthread_exit((void *) (intptr_t) -1);
  }

  c->fd = udp_socket();
  if (c->fd < 0)
  {
    perror("udp_socket");
    pthread_exit((void *) (intptr_t) -1);
  }

  c->txbuf = (__u8 *) malloc(msg_size);
  c->rxbuf = (__u8 *) malloc(msg_size);
  if (!c->txbuf || !c->rxbuf)
  {
    perror("malloc buffers");
    pthread_exit((void *) (intptr_t) -1);
  }
  memset(c->txbuf, 0, msg_size);
  memset(c->rxbuf, 0, msg_size);

  /* Core 0 resolves ARP */
  if (c->id == 0)
  {
    udp_sendto(c->fd, c->txbuf, msg_size, (struct sockaddr *) &c->dst, c->dstlen);
    sleep(1);
    udp_sendto(c->fd, c->txbuf, msg_size, (struct sockaddr *) &c->dst, c->dstlen);
    sleep(1);
    while(udp_poll_fast() == 0);
    while(udp_recvfrom(c->fd, c->rxbuf, msg_size, NULL, 0) == 0);
    fprintf(stderr, "Resolved ARP\n");
  }  

  /* Synchronize start with all other cores and main */
  pthread_barrier_wait(&start_barrier);

  c->txb_load_interval = 0;
  c->txp_load_interval = 0;
  c->txb_tp_interval = 0;
  c->txp_tp_interval = 0;
  c->total_tx_pkts = 0;
  c->total_rx_pkts = 0;
  c->tokens  = 0.0;
  c->last_tsc = util_rdtsc();

  start_tsc = util_rdtsc();
  for (;;)
  {
    elapsed = us_since_tsc(start_tsc);
    if (duration > 0 && elapsed >= (duration * 1000000))
      break;

    /* Update token bucket using TSC */
    __u64 tsc_now = util_rdtsc();
    double delta_cycles = (double)(tsc_now - c->last_tsc);
    double delta_us = delta_cycles / tsc_per_us;
    if (delta_us < 0.0)
      delta_us = 0.0;

    double bytes_per_us = c->rate / 8.0;
    c->tokens += delta_us * bytes_per_us;

    /* Cap bucket to 1 ms worth of traffic to avoid unbounded bursts */
    double max_tokens = bytes_per_us * 1000.0;
    if (c->tokens > max_tokens)
      c->tokens = max_tokens;

    c->last_tsc = tsc_now;

    /* Send as many packets as tokens allow */
    while (c->tokens >= (double) msg_size)
    {
      ph = (struct payload_hdr *) c->txbuf;
      ph->tsc = util_rdtsc();

      udp_poll_fast();
      s = udp_sendto(c->fd, c->txbuf, msg_size,
                    (struct sockaddr *) &c->dst, c->dstlen);
      if (s < 0)
      {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
          break;
        if (errno == EINTR)
          continue;

        perror("sendto");
        pthread_exit((void *)(intptr_t) -1);
      }

      c->tokens -= msg_size;

      c->txb_load_interval += (__u64) s;
      c->txp_load_interval++;
      c->total_tx_pkts++;
    }

    /* Drain replies */
    while (1)
    {
      udp_poll_fast();
      r = udp_recvfrom(c->fd, c->rxbuf, msg_size, NULL, 0);
      if (r < 0)
      {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
          break;

        perror("recvfrom");
        pthread_exit((void *) (intptr_t) -1);
      }

      if ((size_t) r < sizeof(struct payload_hdr))
        continue;

      ph = (struct payload_hdr *) c->rxbuf;
      rtt_us = us_since_tsc(ph->tsc);
      c->txb_tp_interval += (__u64) r;
      c->txp_tp_interval++;
      c->total_rx_pkts++;
      hist_add(rtt_us);
    }
  }


  pthread_exit((void *)(intptr_t)0);
}

int main(int argc, char **argv)
{
  int ret;

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

  ret = udp_connect_slow();
  if (ret != 0)
  {
    fprintf(stderr, "failed to connect to Chamelio");
    exit(EXIT_FAILURE);
  }
  
  /* Calibrate tsc */
  calibrate_tsc();

  /* Allocate cores and thread handles */
  cores = (struct core_ctx *) calloc((size_t) ncores, 
      sizeof(struct core_ctx));
  threads = (pthread_t *) calloc((size_t) ncores, sizeof(pthread_t));
  if (!cores || !threads)
  {
    perror("calloc cores/threads");
    exit(EXIT_FAILURE);
  }

  /* Prepare cores */
  for (int i = 0; i < ncores; i++)
  {
    memset(&cores[i], 0, sizeof(struct core_ctx));
    cores[i].id = i;
    cores[i].rate = rate;
    cores[i].tokens = 0.0;
    cores[i].last_tsc = 0;

    memset(&cores[i].dst, 0, sizeof(cores[i].dst));
    cores[i].dst.sin_family = AF_INET;
    cores[i].dst.sin_port   = htons((__u16) port);
    if (inet_pton(AF_INET, server_ip, &cores[i].dst.sin_addr) != 1)
    {
      fprintf(stderr, "Invalid server_ip\n");
      return EXIT_FAILURE;
    }
    cores[i].dstlen = sizeof(cores[i].dst);
  }

  /* Barrier for all cores + main (to synchronize start of sending) */
  if (pthread_barrier_init(&start_barrier, NULL, 
      (unsigned) ncores + 1) != 0)
  {
    perror("pthread_barrier_init");
    exit(EXIT_FAILURE);
  }

  /* Spawn core threads */
  for (int i = 0; i < ncores; i++)
  {
    int rc = pthread_create(&threads[i], NULL, core_thread, &cores[i]);
    if (rc != 0)
    {
      errno = rc;
      perror("pthread_create");
      exit(EXIT_FAILURE);
    }
  }

  printf("Initialized %d core(s). Waiting for synchronized start...\n", 
      ncores);
  fflush(stdout);

  /* Wait for work threads before continuing */
  pthread_barrier_wait(&start_barrier);
  
  /* Establish timebase and reporting windows, then release the barrier */
  t_start_ns = now_ns();
  t_last_report_ns = t_start_ns;
  t_next_report_ns = t_start_ns + 1000000000ULL;
  t_end_ns = t_start_ns + (__u64) duration * 1000000000ULL;
  
  /* Per-second reporting loop in main thread */
  while (true)
  {
    __u64 now = now_ns();
    if (duration > 0 && now >= t_end_ns)
      break;

    if (now >= t_next_report_ns)
      print_stats(now);

    /* Sleep a bit to avoid busy wait */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 2000000 };
    nanosleep(&ts, NULL);
  }

  /* Join all cores */
  for (int i = 0; i < ncores; i++)
  {
    void *rc = NULL;
    pthread_join(threads[i], &rc);
    (void) rc;
  }

  /* One last print if we overshot the tick */
  print_stats(now_ns());

  /* Final summary */
  __u64 p50 = 0, p99 = 0, p999 = 0;
  bool have = hist_percentiles(&p50, &p99, &p999);

  /* Sum total sent packets across cores */
  __u64 total_tx = 0, total_rx = 0;
  for (int i = 0; i < ncores; i++)
  {
    total_tx += cores[i].total_tx_pkts;
    total_rx += cores[i].total_rx_pkts;
  }

  printf("\n=== Summary ===\n");
  printf("Cores      : %d\n", ncores);
  printf("TX packets : %llu\n", (unsigned long long) total_tx);
  printf("RX packets : %llu\n", (unsigned long long) total_rx);
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

  /* Cleanup */
  for (int i = 0; i < ncores; i++)
  {
    if (cores[i].txbuf) free(cores[i].txbuf);
    if (cores[i].rxbuf) free(cores[i].rxbuf);
  }
  free(cores);
  free(threads);
  free(rtt_hist);
  pthread_barrier_destroy(&start_barrier);

  return 0;
}

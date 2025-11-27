/* Usage:
 *   ./udp_linux_server <bind_ip> <port> [--msize N] [--ncores N] [--buf-size N]
 *
 * Example:
 *   ./udp_linux_server 0.0.0.0 9000 --msize 64 --ncores 2 --buf-size 4096
 * 
 *   --msize Size of expected message
 *   --ncores Number of cores to use
 *   --buf-size Size of transmit buffer
 */

#define _GNU_SOURCE
#include <udp_lib.h>
#include <errno.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <string.h>
#include <sys/socket.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <assert.h>

struct core {
  int id;
  int fd;
  int nrx;
  int ntx;
  __u8 *buf;
  __u64 rxb_interval;
  __u64 rxp_interval;
  __u64 txb_interval;
  __u64 txp_interval;
  struct sockaddr_in dst_addr;
};

/* Default arg values */
size_t msize = 64;
size_t bsize = 4096;
int ncores = 1;

__u64 rxb_interval_total, rxp_interval_total;
__u64 txb_interval_total, txp_interval_total;
__u64 t_start_ns;
__u64 t_last_report_ns, t_next_report_ns;

int port;
const char *server_ip;
static pthread_barrier_t start_barrier;
static pthread_t *threads = NULL;
static struct core *cores = NULL;
struct sockaddr_in src_addr;

static inline __u64 now_ns(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return (__u64) ts.tv_sec * 1000000000ULL + (__u64) ts.tv_nsec;
}

static inline int parse_args(int argc, char **argv)
{
  int i;

  if (argc < 3)
  {
    fprintf(stderr, "Usage: %s <bind_ip> <port>" 
        " [--msize N] [--ncores N] [--buf-size]\n", argv[0]);
    return -1;
  }

  server_ip = argv[1];
  port = atoi(argv[2]);
  if (port <= 0 || port > 65535)
  {
    fprintf(stderr, "Invalid port\n");
    return -1;
  }

  for (i = 3; i < argc; i++)
  {
    if (strcmp(argv[i], "--msize") == 0 && i + 1 < argc)
    {
      msize = (size_t)strtoul(argv[++i], NULL, 10);
    }
    else if (strcmp(argv[i], "--ncores") == 0 && i + 1 < argc)
    {
      ncores = (size_t)strtoul(argv[++i], NULL, 10);
    }
    else if (strcmp(argv[i], "--buf-size") == 0 && i + 1 < argc)
    {
      bsize = (size_t)strtoul(argv[++i], NULL, 10);
    }
    else
    {
      fprintf(stderr, "Unknown arg: %s\n", argv[i]);
      return -1;
    }
  }

  return 0;
}

static inline void print_stats(__u64 now)
{
  double interval_s, rps_rx, mbps_rx, rps_tx, mbps_tx;
  __u64 bytes_rx = 0, pkts_rx = 0, bytes_tx = 0, pkts_tx = 0;

  interval_s = (double) (t_next_report_ns - t_last_report_ns) / 1e9;
  if (interval_s <= 0.0) interval_s = 1.0;

  /* Aggregate per-core and reset per-core intervals */
  for (int i = 0; i < ncores; i++)
  {
    bytes_rx += __sync_fetch_and_add(&cores[i].rxb_interval, 0);
    pkts_rx  += __sync_fetch_and_add(&cores[i].rxp_interval, 0);
    bytes_tx += __sync_fetch_and_add(&cores[i].txb_interval, 0);
    pkts_tx  += __sync_fetch_and_add(&cores[i].txp_interval, 0);
    __sync_fetch_and_and(&cores[i].rxb_interval, 0);
    __sync_fetch_and_and(&cores[i].rxp_interval, 0);
    __sync_fetch_and_and(&cores[i].txb_interval, 0);
    __sync_fetch_and_and(&cores[i].txp_interval, 0);
  }
  
  rxb_interval_total = bytes_rx;
  rxp_interval_total  = pkts_rx;
  txb_interval_total = bytes_tx;
  txp_interval_total  = pkts_tx;

  rps_rx  = (double) rxp_interval_total / interval_s;
  mbps_rx = ((double) rxb_interval_total * 8.0 / 1e6) / interval_s;
  rps_tx  = (double) txp_interval_total / interval_s;
  mbps_tx = ((double) txb_interval_total * 8.0 / 1e6) / interval_s;

  printf("[ %3lus ] rx=%lld rps | rx=%.2f Mb/s | tx=%lld rps | tx=%.2f Mb/s \n",
          (unsigned long)((t_next_report_ns - t_start_ns) / 1000000000ULL),
          (__u64) rps_rx, mbps_rx, (__u64) rps_tx, mbps_tx);
  fflush(stdout);

  t_last_report_ns  = t_next_report_ns;
  t_next_report_ns += 1000000000ULL;

  if (now > t_next_report_ns + 5000000000ULL)
  {
    t_next_report_ns = now + 1000000000ULL;
    t_last_report_ns = now;
  }
}

static inline struct udp_context_lib * core_init(struct core *c)
{
  __u8 *buf;
  int ret, fd, n;
  struct udp_context_lib *udp_ctx;

  buf = (__u8 *) malloc(bsize);
  if (buf == NULL)
  {
    fprintf(stderr, "failed to malloc buf");
    abort();
  }
  c->buf = buf;
  memset(c->buf, 0, bsize);

  udp_ctx = udp_ctx_new();
  if (udp_ctx == NULL)
    abort();

  fd = udp_socket(udp_ctx);
  if (fd < 0)
    abort();

  ret = udp_bind(udp_ctx, fd, (struct sockaddr *) &src_addr, sizeof(src_addr));
  if (ret < 0)
  {
    perror("bind");
    abort();
  }

  /* Core 0 resolves ARP */
  if (c->id == 0)
  {
    while(udp_poll_fast(udp_ctx) == 0){}
    n = udp_recvfrom(udp_ctx, fd, c->buf, msize, 
        (struct sockaddr *) &c->dst_addr, sizeof(c->dst_addr));
    udp_sendto(udp_ctx, fd, c->buf, (size_t) n, 
      (struct sockaddr *) &c->dst_addr, sizeof(c->dst_addr));
    printf("ARP initialized\n");
  }

  c->fd = fd;
  c->nrx = 0;
  c->ntx = 0;
  c->rxb_interval = 0;
  c->rxp_interval = 0;
  c->txb_interval = 0;
  c->txp_interval = 0;
  return udp_ctx;
}

static inline void core_rx(struct udp_context_lib *udp_ctx, struct core *c)
{
  int nrx;
  
  while (c->nrx < bsize)
  {
    assert(msize <= bsize - c->nrx);
    nrx = udp_recvfrom(udp_ctx, c->fd, c->buf + c->nrx, msize, 
        (struct sockaddr *) &c->dst_addr, sizeof(c->dst_addr));
    assert(nrx == msize || nrx < 0);
    
    if (nrx <= 0)
      break;
    
    c->nrx += nrx;
    __sync_fetch_and_add(&c->rxp_interval, 1);
    __sync_fetch_and_add(&c->rxb_interval, msize);
  }
}

static inline void core_tx(struct udp_context_lib *udp_ctx, struct core *c)
{
  int ntx;
  
  while (c->ntx < c->nrx)
  {
    assert(msize <= c->nrx - c->ntx);
    ntx = udp_sendto(udp_ctx, c->fd, c->buf + c->ntx, msize, 
        (struct sockaddr *) &c->dst_addr, sizeof(c->dst_addr));
    assert(ntx == msize || ntx < 0);
          
    if (ntx <= 0)
      break;
      
    c->ntx += ntx;
    __sync_fetch_and_add(&c->txp_interval, 1);
    __sync_fetch_and_add(&c->txb_interval, msize);
  }
  
  c->ntx = 0;
  c->nrx = 0;
}

static inline void * core_thread(void *arg)
{
  struct core *c;
  struct udp_context_lib *udp_ctx;
  
  c = (struct core *) arg;
  udp_ctx = core_init(c);

  /* Synchronize start with all other cores and main */
  pthread_barrier_wait(&start_barrier);

  while (1)
  {
    udp_poll_fast(udp_ctx);
    core_rx(udp_ctx, c);
    udp_poll_fast(udp_ctx);
    core_tx(udp_ctx, c);
  }
  

  pthread_exit((void *)(intptr_t)0);
}

static inline void prepare_cores()
{
  int i;

  for (i = 0; i < ncores; i++)
  {
    memset(&cores[i], 0, sizeof(struct core));
    cores[i].id = i;
    memset(&cores[i].dst_addr, 0, sizeof(cores[i].dst_addr));

    if (inet_pton(AF_INET, server_ip, &src_addr.sin_addr) != 1)
    {
      fprintf(stderr, "Invalid server_ip\n");
      abort();
    }
  }
}

static inline void start_cores()
{
  int i, rc;
  
  /* Spawn core threads */
  for (i = 0; i < ncores; i++)
  {
    rc = pthread_create(&threads[i], NULL, core_thread, &cores[i]);
    if (rc != 0)
    {
      errno = rc;
      perror("pthread_create");
      abort();
    }
  }
}

static inline void init_server()
{
  src_addr.sin_family = AF_INET;
  src_addr.sin_port = htons((__u16) port);
  memset(&src_addr, 0, sizeof(src_addr));

  src_addr.sin_family = AF_INET;
  src_addr.sin_port   = htons((__u16)port);
  if (inet_pton(AF_INET, server_ip, &src_addr.sin_addr) != 1)
  {
    fprintf(stderr, "Invalid bind_ip\n");
    abort();
  }

  /* Allocate cores and thread handles */
  cores = (struct core *) calloc((size_t) ncores, 
      sizeof(struct core));
  threads = (pthread_t *) calloc((size_t) ncores, sizeof(pthread_t));
  if (!cores || !threads)
  {
    perror("calloc cores/threads");
    abort();
  }

  /* Barrier for all cores + main (to synchronize start of sending) */
  if (pthread_barrier_init(&start_barrier, NULL, 
      (unsigned) ncores + 1) != 0)
  {
    perror("pthread_barrier_init");
    abort();
  }
  
  rxb_interval_total = 0;
  rxp_interval_total = 0;
  txb_interval_total = 0;
  txp_interval_total = 0;
}

int main(int argc, char **argv)
{
  int ret, i;
  __u64 now;

  ret = parse_args(argc, argv);
  if (ret != 0)
  {
    fprintf(stderr, "failed to parse args\n");
    abort();
  }

  ret = udp_connect_slow();
  if (ret != 0)
    abort();
    
  init_server();
  prepare_cores();
  start_cores();

  /* Wait for work threads brefore continuing */
  printf("Initialized %d core(s). Waiting for barrier\n", ncores);
  fflush(stdout);
  pthread_barrier_wait(&start_barrier);

  printf("UDP echo server listening on %s:%d (msize=%zu) (bsize=%zu) (ncores=%d)\n", 
      server_ip, port, msize, bsize, ncores);
  fflush(stdout);
  
  /* Establish timebase and reporting windows */
  t_start_ns = now_ns();
  t_last_report_ns = t_start_ns;
  t_next_report_ns = t_start_ns + 1000000000ULL;
  
  /* Per-second reporting loop in main thread */
  while (1)
  {
    now = now_ns();
    if (now >= t_next_report_ns)
      print_stats(now);

    /* Sleep a bit to avoid busy wait */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 2000000 };
    nanosleep(&ts, NULL);
  }
  
  /* Join all cores */
  for (i = 0; i < ncores; i++)
    pthread_join(threads[i], NULL);
    
  return 0;
}

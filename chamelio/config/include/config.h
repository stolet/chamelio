#ifndef CONFIG_H_
#define CONFIG_H_

#include <linux/types.h>

#define MAX_PATH 128

struct configuration {
  /*** SHM configurations ***/
  /* Shared memory size for one guest */
  __u64 shm_len;
  /* Internal Chamelio shared memory size */
  __u64 shm_internal_len;
  /* Size of queue between control and fast path in Chamelio */
  __u64 cham_queue_len;
  /* Size of queue between guest agent and chamelio */
  __u64 agt_queue_len;
  /* Size of transmit queue for control path */
  __u64 control_txq_len;
  /* Size of each packet in control transmit queue */
  __u64 control_txq_pkt_len;

  /*** IP address configurations ***/
  /* IP address for this host */
  __u32 ip;
  /* IP prefix length for this host */
  __u8 ip_prefix;
  /* List of routes */
  struct config_route *routes;

  /*** Max values ***/
  /* Max number of guests supported */
  __u32 max_guests;

  /*** Fast-path configurations ***/
  /* Enable checksum offload */
  __u32 fp_xsumoffloads;
  /* Enable combined infra + eBPF JIT */
  __u32 fp_jit_combined;
  /* Max number of fast-path cores */
  __u32 fp_cores_max;
  
  /*** Performance isolation configurations ***/
  /* Enable performance isolation */
  __u32 perf_iso;
  /* Maximum budget cap per guest in microseconds */
  __u32 perf_iso_cap;
  /* Boost multiplier for budget */
  double perf_iso_boost;

  /*** Virtualization configurations ***/
  /* Use GRE for network virtualization */
  __u32 virt_gre;
  /* Path to network virtualization configuration */
  char virt_path[MAX_PATH];

  /*** DPDK configurations ***/
  /* NUMA node for shared memory allocations */
  int numa_shm;
  /* NUMA node for internal shared memory */
  int numa_shm_internal;
  /* NUMA node for DPDK RX rings */
  int numa_rxring;
  /* NUMA node for DPDK TX rings */
  int numa_txring;
  /* NUMA node for DPDK mbuf pool */
  int numa_mpool;
  /* DPDK extra argument count */
  int dpdk_argc;
  /* DPDK extra argument vector */
  char **dpdk_argv;
};

/* Route entry in configuration */
struct config_route {
  /* Destination IP address */
  __u32 ip;
  /* Destination prefix length */
  __u8 ip_prefix;
  /* Next hop IP */
  __u32 next_hop_ip;
  /* Next pointer for route list */
  struct config_route *next;
};

int config_parse(struct configuration *c, int argc, char **argv);

#endif

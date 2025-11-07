#ifndef CONFIG_H_
#define CONFIG_H_

#include <linux/types.h>

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
  /* Max number of applications per guest */
  __u32 max_apps;
  /* Max number of application contexts per app */
  __u32 max_app_ctxs;
  /* Max number of buffers per application */
  __u32 max_bufs;

  /*** Fast-path configurations ***/
  /* Enable checksum offload */
  __u32 fp_xsumoffloads;
  /* Max number of fast-path cores */
  __u32 fp_cores_max;
  
  /*** DPDK configurations ***/
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
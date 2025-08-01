#ifndef CONFIG_H_
#define CONFIG_H_

#include <stdint.h>

struct configuration {
  /*** SHM configurations ***/
  /* Shared memory size for one guest */
  uint64_t shm_len;
  /* Internal Chamelio shared memory size */
  uint64_t shm_internal_len;
  /* Size of queue between slow and fast path in Chamelio */
  uint64_t cham_queue_len;
  /* Size of queue between application and slow path */
  uint64_t app_queue_len;
  /* Size of RX bump queue between application and slow path */
  uint64_t app_ctx_rx_queue_len;
  /* Size of TX bump queue between application and slow path */
  uint64_t app_ctx_tx_queue_len;
  /* Size of queue between guest agent and chamelio */
  uint64_t agt_queue_len;
  /* Size of application receive buffer */
  uint64_t rxbuf_len;
  /* Size of application transmit buffer */
  uint64_t txbuf_len;

  /*** IP address configurations ***/
  /* IP address for this host */
  uint32_t ip;
  /* IP prefix length for this host */
  uint8_t ip_prefix;
  /* List of routes */
  struct config_route *routes;

  /*** Max values ***/
  /* Max number of guests supported */
  uint32_t max_guests;
  /* Max number of applications per guest */
  uint32_t max_apps;
  /* Max number of application contexts per app */
  uint32_t max_app_ctxs;

  /*** Fast-path configurations ***/
  /* Enable checksum offload */
  uint32_t fp_xsumoffloads;
  /* Max number of fast-path cores */
  uint32_t fp_cores_max;
  
  /*** DPDK configurations ***/
  /* DPDK extra argument count */
  int dpdk_argc;
  /* DPDK extra argument vector */
  char **dpdk_argv;
};

/* Route entry in configuration */
struct config_route {
  /* Destination IP address */
  uint32_t ip;
  /* Destination prefix length */
  uint8_t ip_prefix;
  /* Next hop IP */
  uint32_t next_hop_ip;
  /* Next pointer for route list */
  struct config_route *next;
};

int config_parse(struct configuration *c, int argc, char **argv);

#endif
#ifndef HOMA_CONFIG_H_
#define HOMA_CONFIG_H_

#include <linux/types.h>

/* Slow-path configuration */
struct homa_configuration {
  /* Protocol is running in a VM */
  __u32 virt;
  /* Initial bytes transmitted without receiver authorization. */
  __u32 unscheduled_bytes;
  /* Bytes kept outstanding for the active scheduled receive. */
  __u32 grant_bytes;
  /* Cap on authorized-but-not-received bytes for additional grants. */
  __u32 incoming_bytes;
  /* Aggregate physical link pacing rate in kilobits per second. */
  __u32 link_kbps;
  /* Client RPC slots reserved per application context. */
  __u32 context_rpcs;
};

/* Parse options: 0 on success, -1 on error */
int homa_config_parse(struct homa_configuration *c, int argc, char **argv);

#endif

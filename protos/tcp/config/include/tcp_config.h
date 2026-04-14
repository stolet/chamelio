#ifndef TCP_CONFIG_H_
#define TCP_CONFIG_H_

#include <linux/types.h>

enum tcp_cc_algorithm {
  TCP_CC_ALGO_CONST_RATE = 0,
  TCP_CC_ALGO_DCTCP_RATE,
};

struct tcp_configuration {
  /* Protocol is running in a VM */
  __u32 virt;
  /* Use GRE for network virtualization */
  __u32 virt_gre;
  /* Size of the receive buffer in bytes */
  __u32 rxbuf_sz;
  /* Size of the transmit buffer in bytes */
  __u32 txbuf_sz;
  /* Number of elements in the app queues */
  __u32 appq_len;
  /* Number of elements in the bump queues */
  __u32 bumpq_len;
  /* Number of elements in the control queues */
  __u32 ctlq_len;

  /* Initial RTT estimate in microseconds */
  __u32 cc_rtt_init;
  /* Congestion-control algorithm */
  __u32 cc_algorithm;
  /* Minimum delay between control-loop iterations in microseconds */
  __u32 cc_control_granularity;
  /* Control interval in multiples of RTT */
  __u32 cc_control_interval;
  /* Number of control intervals without ACKs before retransmission */
  __u32 cc_rexmit_ints;
  /* DCTCP EWMA weight scaled by UINT_MAX */
  __u32 cc_dctcp_weight;
  /* Initial DCTCP rate in kbps */
  __u32 cc_dctcp_init;
  /* DCTCP additive increase step in kbps */
  __u32 cc_dctcp_step;
  /* DCTCP multiplicative increase factor minus one scaled by UINT_MAX */
  __u32 cc_dctcp_mimd;
  /* Minimum DCTCP rate in kbps */
  __u32 cc_dctcp_min;
  /* Minimum number of ACKs before processing DCTCP samples */
  __u32 cc_dctcp_minpkts;
  /* Constant rate in kbps, 0 means unlimited */
  __u32 cc_const_rate;
};

int tcp_config_parse(struct tcp_configuration *c, int argc, char **argv);

#endif

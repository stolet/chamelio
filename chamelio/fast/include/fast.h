#ifndef FAST_H_
#define FAST_H_

#include <linux/types.h>

#include <rte_ethdev.h>

#include "cham_fast.h"
#include "nic.h"
#include "nic_fast.h" 
#include "config.h"
#include "arp.h"
#include "fast_stats.h"
#include "ebpf.h"
#include "queue_types.h"
#include "shmalloc.h"

#define FAST_RX_BATCH_SIZE 16
#define FAST_TX_BATCH_SIZE 16
#define FAST_DEQ_BATCH_SIZE 32
#define FAST_CTL_BATCH_SIZE 16

/* We want the TXBUF_SIZE to be the larger or the batch sizes */
#define TXBUF_SIZE 2 * FAST_DEQ_BATCH_SIZE
/* Size of cache for preallocated mbufs used for transmission */
#define TX_CACHE_SIZE 128

struct proto_fast {
  /* Registered protocol type for handwritten fast path */
  __u8 proto_type;
  /* Number of enabled queues */
  __u16 ndqueues;
  /* Head of enabled queues */
  __u16 dqueues_head;
  /* Tail of enabled queues */
  __u16 dqueues_tail;
  /* Array of nodes for enabled queues dequeued by Chamelio */
  struct cham_dqueue dqueues[MAX_PROTO_QUEUES];
  /* Handle containing protocol state passed to custom fast-path */  
  struct cham_ebpf_ctx ebpf_ctx;
  /* RX event handler has been uploaded */
  __u8 has_event_rx;
  /* TX event handler has been uploaded */
  __u8 has_event_tx;
  /* Dequeue event handler has been uploaded */
  __u8 has_event_deq;

  /* Jitted LLVM VM to process one received packet */
  struct ebpf_vm_c *event_rx_vm;
  /* Jitted LLVM VM to process one scheduled packet for transmission */
  struct ebpf_vm_c *event_tx_vm;
  /* Jitted LLVM VM to dequeue and process entry from a queue */
  struct ebpf_vm_c *event_deq_vm;
};

struct guest_fast {
  /* Guest ID */
  __u8 id;
  /* IP assigned to VM */
  __u32 ip;
  /* GRE key used by this guest */
  __u32 gre_key;
  /* Pointer to guest budget counter shared with control */
  __s64 *budget;
  /* Protocol to use with this guest */
  struct proto_fast proto;
  /* Base pointer to shared memory region for this guest */
  void *shm_base;
  /* Length of shared memory region for this guest */
  __u64 shm_len;
};

struct fast_context {
  /* ID of this fast-path context */
  __u8 id;
  /* NIC context for the fast-path */
  struct nic_fast_context nic_ctx;
  /* Chamelio configuration */
  struct configuration *config;
  /* Hot config flags cached locally */
  /* Use combined LLVM IR when jitting */
  __u8 fp_jit_combined;
  /* Select eBPF or handwritten fast-path mode */
  __u8 fp_proto_mode;
  /* Encapsulate packets with GRE */
  __u8 virt_gre;
  /* Enable per-packet budget accounting in fast-path */
  __u8 perf_iso;
  
  /* Number of guests registered so far in this fast-path */
  __u8 n_guests;
  /* First guest to poll in the tx phase */
  __u8 next_tx_guest;
  /* First guest to poll in the queue phase */
  __u8 next_queues_guest;
  /* List of guests registered so far in this fast-path */
  struct guest_fast guests[CHAMELIO_MAX_GUESTS];

  /* Number of mbufs that are processed and ready to be transmitted */
  __u16 tx_n;
  /* List of mbuf pointers that are processed and ready to be transmitted */
  struct rte_mbuf *tx_mbs[TXBUF_SIZE];

  /* Number of preallocated mbufs for transmission */
  __u16 tx_cache_n;
  /* Head of tx cache head */
  __u16 tx_cache_head;
  /* Pointers to preallocated mbufs */
  struct rte_mbuf *tx_cache_mbs[TX_CACHE_SIZE];

  /* Queue from fast-path core to control-path */
  struct equeue *fast_ctl_q;
  /* Queue from the control-path to the fast-path */
  struct dqueue *ctl_fast_q;
  /* Transmit queue for packets from control-path */
  struct dqueue *ctl_txq;
  /* Cumulative batch counters sampled by the control-path */
  struct fast_batch_counters batch_stats;

  /* File descriptor for internal shared memory */
  int shm_fd_internal;
  /* Base pointer for internal shared memory region */
  void *shm_base_internal;
  
  /* ARP table replicated in fast-path to avoid locks */
  struct arp_table arp_table;
  
  /* Read-only after being initialized */
  /* Network virtualization table indexed by GRE key and inner IP */
  struct netvirt_table *inner_table;
  /* Network virtualization table indexed by guest ID and outer IP */
  struct netvirt_table *gid_table;
  /* Aggregate combined entries shared across fast-path cores */
  ebpf_jitted_fn agg_rx_fn;
  ebpf_jitted_fn agg_deq_fn;
  ebpf_jitted_fn agg_tx_fn;
};

/* Initialises the fast-path context when a core is launched */
int fast_context_init(struct fast_context *f_ctx, 
    struct nic_context *nic_ctx, __u16 thread_id,
    struct shm_handle *fc_handle, struct shm_handle *cf_handle,
    struct shm_handle *ctxq_handle, struct configuration *config, 
    int shm_fd_internal, void *shm_base_internal);
/* Dataplane loop in a fast-path core */
int fast_loop(struct fast_context *ctx);
/* Cleans up the fast-path */
void fast_context_destroy();
/* Flushes the transmit buffer by sending packets */
int fast_txflush(struct fast_context *ctx);




#endif

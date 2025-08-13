#ifndef CHAM_LIB_H_
#define CHAM_LIB_H_

#include <stdint.h>

#include "queue.h"

#define GUEST_SOCKET_PATH "guest_socket"
#define IVSHMEM_PROTOCOL_VERSION 0
#define HOST_PEERID 255

struct guest_lib {
  /* Unix socket used to register with Chamelio */
  int uxsocket_fd;
  /* File descriptor for shared memory region used by this guest */
  int shm_fd;
};

struct proto_lib {
  /* Size of shared memory region */
  uint32_t shm_size;
  /* Base pointer for shared memory region used by this guest */
  void *shm_base;
  /* Allocator for shared memory */
  struct shmalloc *alloc;
  /* Guest->control queue */
  struct equeue *guest_ctl_q;
  /* Control->guest queue */
  struct dqueue *ctl_guest_q;
  /* Number of queues */
  uint16_t nqueues;
  /* Number of elements per queue */  
  uint32_t nelems;
  /* Size of element in each queue */
  uint32_t elsize;
  /* Qeueue offsets in shared memory */
  uint64_t queue_offs[MAX_PROTO_QUEUES];
};

/* Mocks a QEMU ivshmem client */
int cham_init_ivshmem();

/* Connects a guest with Chamelio */
struct guest_lib * cham_connect_guest();
/* Creates a new protocol and maps shared memory region */
struct proto_lib* cham_new_proto(struct guest_lib *g, uint32_t shmsize);

/* Creates queues in the shared memory region of the protocol */
int cham_new_queues(struct proto_lib *p, 
    uint16_t nqueues, uint32_t nelems, uint32_t elsize);
/* Creates a new map in the shared memory region of the protocol */
int cham_new_map(struct proto_lib *p, uint32_t nelems, uint32_t elsize);

/* Enables the queue with the given ID on the specified core */
int cham_enable_queue(struct proto_lib *p, uint16_t qid, uint16_t core);
/* Disables the queue with Chamelio */
int cham_disable_queue(struct proto_lib *p, uint16_t qid);
/* Moves the queue to a new core in Chamelio */
int cham_move_queue(struct proto_lib *p, uint16_t qid, uint16_t core);

/* Uploads an eBPF program to Chamelio and register it with the fast-path */
int cham_upload_ebpf(struct proto_lib *p, void *ebpf_bytecode, uint32_t size);

/* Lets user poll the queue with the control plane */
int cham_poll_control(struct proto_lib *p);

#endif
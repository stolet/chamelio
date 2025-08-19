#ifndef CHAM_LIB_H_
#define CHAM_LIB_H_

#include <stdint.h>

#include "queue.h"

/* TODO: Don't duplicate this */
#define APP_SOCKET_PATH "app_socket"
#define GUEST_SOCKET_PATH "guest_socket"

#define IVSHMEM_PROTOCOL_VERSION 0
#define HOST_PEERID 255

struct proto_queue_lib {
  /* ID of this queue */
  uint16_t id;
  /* Size of the queue */
  uint32_t size;
  /* Offset in shared memory to start of queue */
  uint64_t off;
  /* Protocol this queue belongs to */
  struct proto_lib *proto;
};

struct proto_map_lib {
  /* ID of this map */
  uint16_t id;
  /* Number of elements in the map */
  uint32_t nelems;
  /* Size of each element in the map */
  uint32_t elsize;
  /* Offset in shared memory to start of map */
  uint64_t off;
  /* Protocol this map belongs to */
  struct proto_lib *proto;
};

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
  /* Guest this protocol belongs to */
  struct guest_lib *guest;
  /* Number of Chamelio fast-path cores */
  uint32_t n_fp_cores;
  
  /* Guest->control queue */
  struct equeue *guest_ctl_q;
  /* Control->guest queue */
  struct dqueue *ctl_guest_q;

  /* Number of created protocol queues */
  uint16_t nqueues;
  /* Protocol queues created with Chamelio */
  struct proto_queue_lib queues[MAX_PROTO_QUEUES];

  /* Number of maps */
  uint16_t nmaps;
  /* Maps created with Chamelio */
  struct proto_map_lib maps[MAX_PROTO_MAPS];
};

/* Mocks a QEMU ivshmem client */
int cham_init_ivshmem();

/* Connects a guest with Chamelio */
struct guest_lib * cham_connect_guest();
/* Creates a new protocol and maps shared memory region */
struct proto_lib* cham_new_proto(struct guest_lib *g, uint32_t shmsize);

/* Creates a queue of the specified size in the shared memory of the protocol */
struct proto_queue_lib * cham_new_queue(struct proto_lib *p, uint32_t size);
/* Creates a new map in shared memory */
struct proto_map_lib * cham_new_map(struct proto_lib *p, 
    uint32_t nelems, uint32_t elsize);

/* Enables the queue with the given ID on the specified fast-path core */
int cham_enable_queue(struct proto_lib *p, uint16_t qid, uint16_t core);
/* Disables the queue in the fast-path */
int cham_disable_queue(struct proto_lib *p, uint16_t qid, uint16_t core);

/* Uploads an eBPF program to Chamelio and register it with the fast-path */
int cham_upload_ebpf(struct proto_lib *p, void *ebpf_bytecode, uint32_t size);

/* Polls the queue with the control plane */
int cham_poll_control(struct proto_lib *p);

#endif
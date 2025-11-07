#ifndef CHAM_LIB_H_
#define CHAM_LIB_H_

#include <linux/types.h>

#include "queue.h"

#define APP_SOCKET_PATH "app_socket"

/* This socket is only used by apps running on the
   bare machine to connect to Chamelio. They are 
   treated as a new guest and also assigned a new
   shared memory region. Regular guests use the
   guest socket to connect */
#define GUEST_SOCKET_PATH "guest_socket"

#define IVSHMEM_PROTOCOL_VERSION 0
#define HOST_PEERID 255

struct proto_queue_lib {
  /* ID of this queue */
  __u16 id;
  /* Number of elements in the queue */
  __u32 nelems;
  /* Size of elements in the queue */
  __u32 elsize;
  /* Offset in shared memory to start of queue */
  __u64 off;
  /* Protocol this queue belongs to */
  struct proto_lib *proto;
};

struct proto_ebpf_lib{
/* Size of the ebpf program */
  __u32 size;
  /* Offset in shared memory to start of queue */
  __u64 off;
  /* flag indicating arrival of response from the control plane */
  int flag; 
};

struct proto_map_lib {
  /* ID of this map */
  __u16 id;
  /* Number of elements in the map */
  __u32 nelems;
  /* Size of each element in the map */
  __u32 elsize;
  /* Offset in shared memory to start of map */
  __u64 off;
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
  __u32 shm_size;
  /* Base pointer for shared memory region used by this guest */
  void *shm_base;
  /* Allocator for shared memory */
  struct shmalloc *alloc;
  /* Guest this protocol belongs to */
  struct guest_lib *guest;
  /* Number of Chamelio fast-path cores */
  __u32 n_fp_cores;
  /* Local IP address */
  __u32 local_ip;
  
  /* Guest->control queue */
  struct equeue *guest_ctl_q;
  /* Control->guest queue */
  struct dqueue *ctl_guest_q;

  /* Number of created protocol queues */
  __u16 nqueues;
  /* Protocol queues created with Chamelio */
  struct proto_queue_lib queues[MAX_PROTO_QUEUES];

  /* Number of maps */
  __u16 nmaps;
  /* Maps created with Chamelio */
  struct proto_map_lib maps[MAX_PROTO_MAPS];

  /* One eBPF program per protocol for now */
  struct proto_ebpf_lib ebpf_program;
};

/* Mocks a QEMU ivshmem client */
int cham_init_ivshmem();

/* Connects a guest with Chamelio */
struct guest_lib * cham_connect_guest();
/* Creates a new protocol and maps shared memory region */
struct proto_lib* cham_new_proto(struct guest_lib *g, __u32 shmsize);

/* Creates a queue of the specified size in the shared memory of the protocol */
struct proto_queue_lib * cham_new_queue(struct proto_lib *p, 
    __u32 nelems, __u32 elsize);
/* Creates a new map in shared memory */
struct proto_map_lib * cham_new_map(struct proto_lib *p, 
    __u32 nelems, __u32 elsize);

/* Enables the queue with the given ID on the specified fast-path core */
int cham_enable_queue(struct proto_lib *p, __u16 qid, __u16 core);
/* Disables the queue in the fast-path */
int cham_disable_queue(struct proto_lib *p, __u16 qid, __u16 core);

/* Allocates space for an eBPF program in shared memory and registers it with the control plane */
struct proto_ebpf_lib *cham_allocate_ebpf(struct proto_lib *p, __u32 size);
/* Uploads an eBPF program to Chamelio and register it with the fast-path */
int cham_upload_ebpf(struct proto_lib *p, void *ebpf_bytecode, __u32 size);
/* Frees the allocated eBPF program in shared memory and unregisters it from the control plane */
int cham_free_ebpf(struct proto_lib *p);

/* Polls the queue with the control plane */
int cham_poll_control(struct proto_lib *p);

#endif
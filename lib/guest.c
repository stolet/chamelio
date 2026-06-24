#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <string.h>

#include "include/cham_lib.h"
#include "queue_fns.h"
#include "queue_types.h"
#include "log.h"
#include "shmalloc.h"
#include "uxsocket.h"
#include "vfio.h"

static int handle_proto_res(struct proto_lib *p, struct queue_entry *qe);
static int handle_new_queue_res(struct proto_lib *p, struct queue_entry *qe);
static int handle_new_map_res(struct proto_lib *p, struct queue_entry *qe);
static int handle_allocate_ebpf_res(struct proto_lib *p, struct queue_entry *qe);
static int handle_free_ebpf_res(struct proto_lib *p, struct queue_entry *qe);
static int handle_upload_ebpf_res(struct proto_lib *p, struct queue_entry *qe);

struct guest_lib *cham_connect_guest()
{
  struct guest_lib *g;
  struct sockaddr_un s_un;
  int shm_fd, ret, sock_fd;
  int64_t tmp;

  sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock_fd < 0)
  {
    LOG_ERROR("failed to create socket");
    return NULL;
  }

  s_un.sun_family = AF_UNIX;
  ret = snprintf(s_un.sun_path, sizeof(s_un.sun_path),
                 "%s", GUEST_SOCKET_PATH);
  if (ret < 0 || ret >= sizeof(s_un.sun_path))
  {
    LOG_ERROR("could not copy unix socket path");
    goto close_sockfd;
  }

  if (connect(sock_fd, (struct sockaddr *)&s_un, sizeof(s_un)) < 0)
  {
    LOG_ERROR("cannot connect to chamelio, %s", GUEST_SOCKET_PATH);
    perror("");
    goto close_sockfd;
  }
  LOG_DEBUG("connected to chamelio unix socket %s", GUEST_SOCKET_PATH);

  /* Get shared mem fd */
  if (uxsocket_read_one_msg(sock_fd, &tmp, &shm_fd) < 0 || tmp != -1 || shm_fd < 0)
  {
    if (shm_fd >= 0)
      close(shm_fd);

    LOG_ERROR("cannot read shared memory fd from chamelio");
    goto close_sockfd;
  }
  LOG_DEBUG("received shared memory fd=%d from chamelio", shm_fd);

  g = malloc(sizeof(struct guest_lib));
  if (g == NULL)
  {
    LOG_ERROR("failed to allocate guest_lib struct");
    goto close_sockfd;
  }

  g->uxsocket_fd = sock_fd;
  g->shm_fd = shm_fd;

  return g;

close_sockfd:
  close(sock_fd);
  return NULL;
}

struct proto_lib *cham_new_proto_bare(struct guest_lib *g, __u8 proto_type)
{
  int ret;
  ssize_t sz, off;
  void *shm_base;
  struct proto_lib *p;
  struct equeue *eq;
  struct dqueue *dq;
  struct queue_new_proto_res *res;
  struct shm_allocator *alloc;
  struct shm_handle *sh;
  __u8 resp_buf[sizeof(*res)];
  struct queue_new_proto_req req = {
      .proto_type = proto_type,
  };

  /* Send request on kernel socket */
  struct iovec iov = {
      .iov_base = &req,
      .iov_len = sizeof(req),
  };

  struct msghdr msg = {
      .msg_name = NULL,
      .msg_namelen = 0,
      .msg_iov = &iov,
      .msg_iovlen = 1,
      .msg_control = NULL,
      .msg_controllen = 0,
      .msg_flags = 0,
  };

  sz = sendmsg(g->uxsocket_fd, &msg, 0);
  if (sz != sizeof(req))
  {
    LOG_ERROR("failed to send msg to register new protocol");
    perror("");
    return NULL;
  }

  /* Receive response on kernel socket */
  res = (struct queue_new_proto_res *)resp_buf;
  off = 0;
  while (off < sizeof(*res))
  {
    sz = read(g->uxsocket_fd, (__u8 *)res + off, sizeof(*res) - off);
    if (sz < 0)
    {
      LOG_ERROR("read failed");
      perror("");
      return NULL;
    }
    off += sz;
  }

  if (res->success == 0)
  {
    LOG_ERROR("protocol registration rejected by Chamelio");
    return NULL;
  }
  LOG_DEBUG("protocol type=%u registered: shm_len=%lu n_fp_cores=%u local_ip=%08x",
      proto_type, (unsigned long) res->shm_len, res->n_fp_cores, res->local_ip);

  shm_base = mmap(NULL, res->shm_len, PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_POPULATE, g->shm_fd, 0);
  if (shm_base == (void *)-1)
  {
    LOG_ERROR("failed to map shm region");
    return NULL;
  }

  p = malloc(sizeof(struct proto_lib));
  if (p == NULL)
  {
    LOG_ERROR("failed to allocate proto struct");
    return NULL;
  }
  p->nqueues = 0;
  p->shm_base = shm_base;
  p->shm_size = res->shm_len;
  p->nmaps = 0;
  p->n_fp_cores = res->n_fp_cores;
  p->local_ip = res->local_ip;
  p->shm_fd = g->shm_fd;
  p->shm_off = 0;

  /* Create allocator that manages shared memory */
  alloc = shmalloc_init(shm_base, res->shm_len);
  if (alloc == NULL)
  {
    LOG_ERROR("failed to create allocator");
    return NULL;
  }

  /* Create queue for messages from guest to the control-path */
  ret = shmalloc_alloc(alloc, GUESTQ_NELEMS * GUESTQ_ELSIZE, &sh);
  if (ret != 0)
  {
    LOG_ERROR("failed to allocate memory for queue");
    return NULL;
  }

  eq = equeue_new(GUESTQ_NELEMS, GUESTQ_ELSIZE, sh->addr, sh->off);
  if (eq == NULL)
  {
    LOG_ERROR("failed to create queue");
    return NULL;
  }
  p->guest_ctl_q = eq;

  /* Queue from the guest to the control path should always
     be the first thing allocated in the shared memory region. */
  assert(eq->off == 0);

  /* Create queue for messages from control-path to the guest */
  ret = shmalloc_alloc(alloc, GUESTQ_NELEMS * GUESTQ_ELSIZE, &sh);
  if (ret != 0)
  {
    LOG_ERROR("failed to allocate memory for queue");
    return NULL;
  }

  dq = dqueue_new(GUESTQ_NELEMS, GUESTQ_ELSIZE, sh->addr, sh->off);
  if (dq == NULL)
  {
    LOG_ERROR("failed to allocate memory for queue");
    return NULL;
  }
  p->ctl_guest_q = dq;

  /* Queue from the control path to the guest should always be the
     second thing allocated in the shared memory region */
  assert(dq->off == (GUESTQ_NELEMS * GUESTQ_ELSIZE));

  return p;
}

struct proto_lib *cham_new_proto_virt(__u8 proto_type)
{
  int ret;
  struct proto_lib *p;
  struct equeue *eq;
  struct dqueue *dq;
  struct shm_allocator *alloc;
  struct shm_handle *sh;
  struct queue_entry *qe;
  struct queue_new_proto_req *req;
  struct vfio vfio;

  ret = vfio_init(&vfio);
  if (ret != 0)
  {
    LOG_ERROR("failed to init vfio");
    return NULL;
  }

  p = malloc(sizeof(struct proto_lib));
  if (p == NULL)
  {
    LOG_ERROR("failed to allocate proto struct");
    return NULL;
  }
  p->nqueues = 0;
  p->shm_base = vfio.shm_base;
  p->nmaps = 0;
  p->n_fp_cores = (__u32) -1;
  p->shm_fd = vfio.dev;
  p->shm_off = vfio.shm_off;

  /* Create allocator that manages shared memory */
  alloc = shmalloc_init(vfio.shm_base, vfio.shm_size);
  if (alloc == NULL)
  {
    LOG_ERROR("failed to create allocator");
    return NULL;
  }

  /* Create queue for messages from guest to the control-path */
  ret = shmalloc_alloc(alloc, GUESTQ_NELEMS * GUESTQ_ELSIZE, &sh);
  if (ret != 0)
  {
    LOG_ERROR("failed to allocate memory for queue");
    return NULL;
  }

  eq = equeue_new(GUESTQ_NELEMS, GUESTQ_ELSIZE, sh->addr, sh->off);
  if (eq == NULL)
  {
    LOG_ERROR("failed to create queue");
    return NULL;
  }
  p->guest_ctl_q = eq;

  /* Queue from the guest to the control path should always
     be the first thing allocated in the shared memory region. */
  assert(eq->off == 0);

  /* Create queue for messages from control-path to the guest */
  ret = shmalloc_alloc(alloc, GUESTQ_NELEMS * GUESTQ_ELSIZE, &sh);
  if (ret != 0)
  {
    LOG_ERROR("failed to allocate memory for queue");
    return NULL;
  }

  dq = dqueue_new(GUESTQ_NELEMS, GUESTQ_ELSIZE, sh->addr, sh->off);
  if (dq == NULL)
  {
    LOG_ERROR("failed to allocate memory for queue");
    return NULL;
  }
  p->ctl_guest_q = dq;

  /* Queue from the control path to the guest should always be the
     second thing allocated in the shared memory region */
  assert(dq->off == (GUESTQ_NELEMS * GUESTQ_ELSIZE));

  /* Send message to control to register protocol */
  qe = queue_tail(eq);
  if (qe == NULL)
  {
    LOG_ERROR("failed to get enqueue tail");
    return NULL;
  }
  req = &qe->data.new_proto_req;
  req->proto_type = proto_type;

  ret = queue_enqueue(eq, QUEUE_NEW_PROTO_REQ);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue new proto request");
    return NULL;
  }

  /* Wait to get protocol response */
  while (p->n_fp_cores == (__u32) -1)
    cham_poll_control(p);

  if (p->shm_size == 0)
  {
    LOG_ERROR("protocol registration rejected by Chamelio");
    return NULL;
  }

  return p;
}

struct proto_queue_lib * cham_new_queue(struct proto_lib *p, 
    __u32 nelems, __u32 elsize)
{
  int ret;
  __u16 nqueues;
  struct equeue *q;
  struct queue_entry *qe;
  struct queue_new_queue_req *req;
  struct proto_queue_lib *pq;

  nqueues = p->nqueues;
  q = p->guest_ctl_q;
  qe = queue_tail(q);
  if (qe == NULL)
  {
    LOG_ERROR("failed to get queue tail");
    return NULL;
  }

  req = &qe->data.new_queue_req;
  req->nelems = nelems;
  req->elsize = elsize;
  pq = &p->queues[nqueues];
  req->opaque = (__u64) pq;

  /* Set to 0 so we can check queue was initialised when we poll control */
  pq->elsize = 0;
  pq->nelems = 0;

  ret = queue_enqueue(q, QUEUE_NEW_QUEUE_REQ);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue request for new queues");
    return NULL;
  }

  /* Poll waiting for response */
  while (pq->nelems == 0)
    cham_poll_control(p);

  LOG_DEBUG("queue allocated: qid=%u nelems=%u elsize=%u off=%lu",
      pq->id, pq->nelems, pq->elsize, (unsigned long) pq->off);
  return &p->queues[nqueues];
}

struct proto_map_lib *cham_new_map(struct proto_lib *p,
                                   __u32 nelems, __u32 elsize)
{
  int ret;
  __u16 nmaps;
  struct equeue *q;
  struct queue_entry *qe;
  struct queue_new_map_req *req;
  struct proto_map_lib *m;
 
  nmaps = p->nmaps;
  q = p->guest_ctl_q;
  qe = queue_tail(q);
  if (qe == NULL)
  {
    LOG_ERROR("failed to get queue tail");
    return NULL;
  }

  req = &qe->data.new_map_req;
  req->nelems = nelems;
  req->elsize = elsize;
  m = &p->maps[nmaps];
  req->opaque = (__u64) m;

  /* Set to 0 so we can check map was initialised when we poll control */
  m->nelems = 0;

  ret = queue_enqueue(q, QUEUE_NEW_MAP_REQ);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue request for new map");
    return NULL;
  }
  p->nmaps++;

  /* Poll waiting for response */
  while (m->nelems == 0)
    cham_poll_control(p);

  LOG_DEBUG("map allocated: id=%u nelems=%u elsize=%u off=%lu",
      m->id, m->nelems, m->elsize, (unsigned long) m->off);
  return &p->maps[nmaps];
}

int cham_enable_queue(struct proto_lib *p, __u16 qid, __u16 core)
{
  int ret;
  struct equeue *q;
  struct queue_entry *qe;
  struct queue_enableq_req *req;

  q = p->guest_ctl_q;
  qe = queue_tail(q);
  if (qe == NULL)
  {
    LOG_ERROR("failed to get queue tail");
    return -1;
  }

  req = &qe->data.enableq_req;
  req->qid = qid;
  req->core = core;
  
  ret = queue_enqueue(q, QUEUE_ENABLEQ_REQ);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue request to enable queue");
    return -1;
  }

  return 0;
}

int cham_disable_queue(struct proto_lib *p, __u16 qid, __u16 core)
{
  int ret;
  struct equeue *q;
  struct queue_entry *qe;
  struct queue_disableq_req *req;

  q = p->guest_ctl_q;
  qe = queue_tail(q);
  if (qe == NULL)
  {
    LOG_ERROR("failed to get queue tail");
    return -1;
  }

  req = &qe->data.disableq_req;
  req->qid = qid;
  req->core = core;

  ret = queue_enqueue(q, QUEUE_DISABLEQ_REQ);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue request to disable queue");
    return -1;
  }

  return 0;
}

struct proto_ebpf_lib *cham_allocate_ebpf(struct proto_lib *p, __u32 size)
{
  struct equeue *q = p->guest_ctl_q;
  struct queue_entry *qe = queue_tail(q);
  if (qe == NULL)
  {
    LOG_ERROR("failed to get queue tail");
    return NULL;
  }
  struct queue_allocate_ebpf_req *req = &qe->data.alloc_ebpf_req;
  req->size = size;
  req->opaque = (__u64)&p->ebpf_program;
  p->ebpf_program.flag = 0; //reset flag to 0 before sending request

  int ret = queue_enqueue(q, QUEUE_ALLOCATE_EBPF_REQ);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue request to allocate eBPF program");
    return NULL;
  }

  while(p->ebpf_program.flag == 0)
    cham_poll_control(p);

  LOG_DEBUG("eBPF program allocated: size=%u off=%lu",
      p->ebpf_program.size, (unsigned long) p->ebpf_program.off);
  return &p->ebpf_program;
}

int cham_upload_ebpf(struct proto_lib *p, void *ebpf_bytecode, __u32 size)
{
  if (p->ebpf_program.size == 0) 
  {
    LOG_ERROR("tried to upload eBPF program before allocating it");
    return -1;
  }
  
  /* Copy ebpf bytecode to shared memory */
  void *shm_addr = (__u8 *) p->shm_base + p->ebpf_program.off;
  memcpy(shm_addr, ebpf_bytecode, p->ebpf_program.size);

  struct equeue *q = p->guest_ctl_q;
  struct queue_entry *qe = queue_tail(q);
  if (qe == NULL)
  {
    LOG_ERROR("failed to get queue tail");
    return -1;
  }

  struct queue_up_ebpf_req *req = &qe->data.up_ebpf_req;
  req->size = p->ebpf_program.size;
  req->off = p->ebpf_program.off;
  p->ebpf_program.flag = 0;

  int ret = queue_enqueue(q, QUEUE_UPLOAD_EBPF_REQ);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue request to upload eBPF program");
    return ret;
  }

  while (p->ebpf_program.flag == 0)
    cham_poll_control(p);
  
  if (p->ebpf_program.flag < 0)
  {
    LOG_ERROR("eBPF upload failed");
    return -1;
  }

  LOG_DEBUG("eBPF program uploaded successfully: size=%u off=%lu",
      p->ebpf_program.size, (unsigned long) p->ebpf_program.off);
  return 0;
}

int cham_free_ebpf(struct proto_lib *p)
{
  int ret;
  struct equeue *q;
  struct queue_entry *qe;
  struct queue_free_ebpf_req *req;

  if (p->ebpf_program.size == 0) 
    return 0;
  
  q = p->guest_ctl_q;
  qe = queue_tail(q);
  if (qe == NULL)
  {
    LOG_ERROR("failed to get queue tail");
    return -1;
  }

  req = &qe->data.free_ebpf_req;
  req->size = p->ebpf_program.size;
  req->off = p->ebpf_program.off;
  p->ebpf_program.flag = 0; //reset flag to 0 before sending request

  ret = queue_enqueue(q, QUEUE_FREE_EBPF_REQ);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue request to free eBPF program");
    return ret;
  }

  while (p->ebpf_program.flag == 0)
    cham_poll_control(p);
  
  if (p->ebpf_program.flag < 0) // free failed in control plane
    return -1;
  
  p->ebpf_program.size = 0;
  p->ebpf_program.off = 0;
  p->ebpf_program.flag = 0;

  return ret;
}

int cham_poll_control(struct proto_lib *p)
{
  struct dqueue *q;
  struct queue_entry *qe;

  q = p->ctl_guest_q;
  qe = queue_head(q);

  /* Queue is empty */
  if (qe == NULL)
    return -1;

  switch (qe->type)
  {
    case QUEUE_NEW_PROTO_RES:
      handle_proto_res(p, qe);
      queue_dequeue(q);
      return QUEUE_NEW_PROTO_RES;
    case QUEUE_NEW_QUEUE_RES:
      handle_new_queue_res(p, qe);
      queue_dequeue(q);
      return QUEUE_NEW_QUEUE_RES;
    case QUEUE_NEW_MAP_RES:
      handle_new_map_res(p, qe);
      queue_dequeue(q);
      return QUEUE_NEW_MAP_RES;
    case QUEUE_ALLOCATE_EBPF_RES:
      handle_allocate_ebpf_res(p, qe);
      queue_dequeue(q);
      return QUEUE_ALLOCATE_EBPF_RES;
    case QUEUE_FREE_EBPF_RES:
      handle_free_ebpf_res(p, qe);
      queue_dequeue(q);
      return QUEUE_FREE_EBPF_RES;
    case QUEUE_UPLOAD_EBPF_RES:
      handle_upload_ebpf_res(p, qe);
      queue_dequeue(q);
      return QUEUE_UPLOAD_EBPF_RES;
    default:
      LOG_ERROR("unknown queue entry type from "
                "guest to control-path type=%d",
                qe->type);
      abort();
  }

  return 0;
}

static int handle_proto_res(struct proto_lib *p, struct queue_entry *qe)
{
  struct queue_new_proto_res *res = &qe->data.new_proto_res;

  if (res->success == 0)
  {
    p->n_fp_cores = 0;
    p->local_ip = 0;
    p->shm_size = 0;
    return -1;
  }

  p->n_fp_cores = res->n_fp_cores;
  p->local_ip = res->local_ip;
  p->shm_size = res->shm_len;
  return 0;
}

static int handle_free_ebpf_res(struct proto_lib *p, struct queue_entry *qe)
{
  struct queue_free_ebpf_res *res = &qe->data.free_ebpf_res;

  if (res->success == 0)
  {
    LOG_ERROR("failed to free eBPF program in control plane");
    p->ebpf_program.flag = -1;
  }
  else 
  {
    p->ebpf_program.flag = 1;
  }
  
  return 0;
}

static int handle_upload_ebpf_res(struct proto_lib *p, struct queue_entry *qe)
{
  struct queue_up_ebpf_res *res = &qe->data.up_ebpf_res;

  if (res->success == 0)
  {
    LOG_ERROR("failed to upload eBPF program in control plane");
    p->ebpf_program.flag = -1;
  }
  else 
  {
    p->ebpf_program.flag = 1;
  }
  
  return 0;
}

static int handle_allocate_ebpf_res(struct proto_lib *p, struct queue_entry *qe)
{
  struct queue_allocate_ebpf_res *res;
  struct proto_ebpf_lib *e;

  res = &qe->data.alloc_ebpf_res;
  e = (struct proto_ebpf_lib *) res->opaque;  //placeholder cookie
  e->size = res->size;
  e->off = res->off;
  e->flag = 1; // set flag to 1 to indicate allocation is complete

  return 0;
}

static int handle_new_queue_res(struct proto_lib *p, struct queue_entry *qe)
{
  struct queue_new_queue_res *res;
  struct proto_queue_lib *q;

  res = &qe->data.new_queue_res;
  q = (struct proto_queue_lib *) res->opaque;
  q->id = res->qid;
  q->off = res->off;
  q->elsize = res->elsize;
  q->nelems = res->nelems;
  q->proto = p;
  p->nqueues++;

  return 0;
}

static int handle_new_map_res(struct proto_lib *p, struct queue_entry *qe)
{
  struct queue_new_map_res *res;
  struct proto_map_lib *m;

  res = &qe->data.new_map_res;
  m = (struct proto_map_lib *) res->opaque;
  m->id = res->id;
  m->off = res->off;
  m->nelems = res->nelems;
  m->elsize = res->elsize;
  m->proto = p;
  p->nmaps++;

  return 0;
}

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <sys/mman.h>

#include "cham_lib.h"
#include "queue.h"
#include "log.h"
#include "uxsocket.h"

static int handle_new_queue_res(struct proto_lib *p, struct queue_entry *qe);
int handle_new_map_res(struct proto_lib *p, struct queue_entry *qe);

struct guest_lib * cham_connect_guest()
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

  /* Get shared mem fd */
  if (uxsocket_read_one_msg(sock_fd, &tmp, &shm_fd) < 0 || tmp != -1 || shm_fd < 0) 
  {
    if (shm_fd >= 0) 
      close(shm_fd);
    
    LOG_ERROR("cannot read shared memory fd from chamelio");
    goto close_sockfd;
  }

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

struct proto_lib * cham_new_proto(struct guest_lib *g, uint32_t shmsize)
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
  uint8_t resp_buf[sizeof(*res)];
  struct queue_new_proto_req req = {
    .proto_type = 1,
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
  res = (struct queue_new_proto_res *) resp_buf;
  off = 0;
  while (off < sizeof(*res)) 
  {
    sz = read(g->uxsocket_fd, (uint8_t *) res + off, sizeof(*res) - off);
    if (sz < 0) 
    {
      LOG_ERROR("read failed");
      perror("");
      return NULL;
    }
    off += sz;
  }

  shm_base = mmap(NULL, res->shm_len, PROT_READ | PROT_WRITE, 
      MAP_SHARED | MAP_POPULATE, g->shm_fd, 0);
  if (shm_base == (void *) -1) 
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
  p->guest = g;
  p->n_fp_cores = res->n_fp_cores;

  /* Create allocator that manages shared memory */
  alloc = shmalloc_init(g->shm_fd, shm_base, res->shm_len);
  if (alloc == NULL)
  {
    LOG_ERROR("failed to create allocator");
    return NULL;
  }

  /* Create queue for messages from guest to the control-path */
  ret = shmalloc_alloc(alloc, res->guestq_len, &sh);
  if (ret != 0)
  {
    LOG_ERROR("failed to allocate memory for queue");
    return NULL;
  }

  eq = equeue_new(sh->len, sh->addr, sh->off);
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
  ret = shmalloc_alloc(alloc, res->guestq_len, &sh);
  if (ret != 0)
  {
    LOG_ERROR("failed to allocate memory for queue");
    return NULL;
  }

  dq = dqueue_new(sh->len, sh->addr, sh->off);
  if (dq == NULL)
  {
    LOG_ERROR("failed to allocate memory for queue");
    return NULL;
  }
  p->ctl_guest_q = dq;

  /* Queue from the control path to the guest should always be the
     second thing allocated in the shared memory region */
  assert(dq->off == res->guestq_len);

  return p;
}

struct proto_queue_lib * cham_new_queue(struct proto_lib *p, uint32_t size)
{
  int ret;
  uint16_t nqueues;
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
  req->size = size;
  pq = &p->queues[nqueues];
  req->opaque = (uint64_t) pq;

  /* Set to 0 so we can check it was initialised when we poll control */
  pq->size = 0;

  ret = queue_enqueue(q, QUEUE_NEW_QUEUE_REQ);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue request for new queues");
    return NULL;
  }
  p->nqueues++;

  /* Poll waiting for response */
  /* TODO: Make this async instead of blocking here */
  while (pq->size == 0)
    cham_poll_control(p);

  return &p->queues[nqueues];
}

struct proto_map_lib * cham_new_map(struct proto_lib *p, 
    uint32_t nelems, uint32_t elsize)
{
  int ret;
  uint16_t nmaps;
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
  req->opaque = (uint64_t) m;

  /* Set to 0 so we can check it was initialised when we poll control */
  m->nelems = 0;

  ret = queue_enqueue(q, QUEUE_NEW_MAP_REQ);
  if (ret != 0)
  {
    LOG_ERROR("failed to enqueue request for new map");
    return NULL;
  }
  p->nmaps++;
  
  /* Poll waiting for response */
  /* TODO: Make this async instead of blocking here */
  while (m->nelems == 0)
    cham_poll_control(p);

  return &p->maps[nmaps];;
}

int cham_enable_queue(struct proto_lib *p, uint16_t qid, uint16_t core)
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

int cham_disable_queue(struct proto_lib *p, uint16_t qid, uint16_t core)
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
    case QUEUE_NEW_QUEUE_RES:
      handle_new_queue_res(p, qe);
      queue_dequeue(q);
      return QUEUE_NEW_QUEUE_RES;
    case QUEUE_NEW_MAP_RES:
      handle_new_map_res(p, qe);
      queue_dequeue(q);
      return QUEUE_NEW_MAP_RES;
    default:
      LOG_ERROR("unknown queue entry type from "
          "guest to control-path type=%d", qe->type);
      abort();
  }

  return 0;  
}

int handle_new_queue_res(struct proto_lib *p, struct queue_entry *qe)
{
  struct queue_new_queue_res *res;
  struct proto_queue_lib *q;

  res = &qe->data.new_queue_res;
  q = (struct proto_queue_lib *) res->opaque;
  q->id = res->qid;
  q->off = res->off;
  q->size = res->size;
  q->proto = p;
  p->nqueues++;

  return 0;  
}

int handle_new_map_res(struct proto_lib *p, struct queue_entry *qe)
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
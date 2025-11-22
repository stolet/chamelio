#include "fast.h"
#include "queue_types.h"
#include "queue_fns.h"
#include "scheduler_fns.h"
#include "arp_hdr.h"
#include "txcache.h"

static void handle_new_guest(struct fast_context *ctx, struct queue_entry *qe);
static void handle_new_queue(struct fast_context *ctx, struct queue_entry *qe);
static void handle_new_map(struct fast_context *ctx, struct queue_entry *qe);
static void handle_enableq(struct fast_context *ctx, struct queue_entry *qe);
static void handle_disableq(struct fast_context *ctx, struct queue_entry *qe);
static void handle_upload_ebpf(struct fast_context *ctx, struct queue_entry *qe);

static void handle_arp_tx_pkt(struct fast_context *ctx, struct queue_entry *qe);
static void handle_arp_update(struct fast_context *ctx, struct queue_entry *qe);

int controlif_poll(struct fast_context *ctx)
{
  int i, n;
  __u8 type;
  struct dqueue *q;
  struct queue_entry *qe;
 
  /* TODO: Might be good to decouple messages with control
     information from from messages that want to send packets */
  n = BATCH_SIZE;
  if (TXBUF_SIZE - ctx->tx_n < n)
    n = TXBUF_SIZE - ctx->tx_n;
  
  q = ctx->ctl_fast_q;
  for (i = 0; i < n; i++)
  {
    qe = queue_head(q);

    if (qe == NULL)
      return 0;
    
    type = qe->type;
    switch (type)
    {
      case QUEUE_EMPTY:
        break;
      case QUEUE_NEW_GUEST_REQ:
        handle_new_guest(ctx, qe);
        queue_dequeue(q);
        break;
      case QUEUE_NEW_MAP_REQ:
        handle_new_map(ctx, qe);
        queue_dequeue(q);
        break;
      case QUEUE_NEW_QUEUE_REQ:
        handle_new_queue(ctx, qe);
        queue_dequeue(q);
        break;
      case QUEUE_ENABLEQ_REQ:
        handle_enableq(ctx, qe);
        queue_dequeue(q);
        break;
      case QUEUE_DISABLEQ_REQ:
        handle_disableq(ctx, qe);
        queue_dequeue(q);
        break;
      case QUEUE_UPLOAD_EBPF_REQ:
        handle_upload_ebpf(ctx, qe);
        queue_dequeue(q);
        break;
      case QUEUE_ARP_TX_PKT:
        handle_arp_tx_pkt(ctx, qe);
        queue_dequeue(q);
        break;
      case QUEUE_ARP_UPDATE:
        handle_arp_update(ctx, qe);
        queue_dequeue(q);
        break;
      default:
        LOG_WARN("unknown queue entry type from control path " 
            "to fast path type=%d", type);
        break;
    }
  }

  return 0;
}

static void handle_new_guest(struct fast_context *ctx, struct queue_entry *qe)
{
  struct guest_fast *g;
  struct queue_new_guest_req *req = &qe->data.new_guest_req;

  g = &ctx->guests[req->id];
  g->id = req->id;
  g->shm_base = req->shm_base;
  g->shm_len = req->shm_len;
  
  /* TODO: Have a separate message to initialise protocol */
  g->proto.ndqueues = 0;
  g->proto.dqueues_head = PROTOQ_ID_INVALID;
  g->proto.dqueues_tail = PROTOQ_ID_INVALID;
  
  g->proto.event_rx_vm = NULL;
  g->proto.event_tx_vm = NULL;
  g->proto.event_deq_vm = NULL;
  
  /* Init qman */
  sched_init(&g->proto.ebpf_ctx.sched);
  g->proto.ebpf_ctx.shm_base = g->shm_base;
  g->proto.ebpf_ctx.nmaps = 0;

  ctx->n_guests++;
}

static void handle_new_queue(struct fast_context *ctx, struct queue_entry *qe)
{
  struct guest_fast *g;
  struct proto_fast *p;
  struct cham_equeue *q;
  
  struct queue_new_queue_req *req = &qe->data.new_queue_req;

  g = &ctx->guests[req->gid];
  p = &g->proto;
  q = &p->ebpf_ctx.equeues[req->qid];
  q->id = req->qid;

  equeue_init(&q->eq, req->nelems, req->elsize, g->shm_base + req->off, req->off);
  LOG_DEBUG("created queue qid=%d in fast-path", req->qid);
}

static void handle_new_map(struct fast_context *ctx, struct queue_entry *qe)
{
  struct guest_fast *g;
  struct proto_fast *p;
  struct cham_map *m;
  
  struct queue_new_map_req *req = &qe->data.new_map_req;

  g = &ctx->guests[req->gid];
  p = &g->proto;
  m = &p->ebpf_ctx.maps[p->ebpf_ctx.nmaps];
  p->ebpf_ctx.nmaps++;

  m->id = req->mid;
  m->elsize = req->elsize;
  m->nelems = req->nelems;
  m->off = req->off;
  m->addr = g->shm_base + req->off;
  LOG_DEBUG("created map qid=%d in fast-path", req->mid);
}

static void handle_enableq(struct fast_context *ctx, struct queue_entry *qe)
{
  struct guest_fast *g;
  struct proto_fast *p;
  struct queue_enableq_req *req;
  struct cham_dqueue *q;
  
  req = &qe->data.enableq_req;
  g = &ctx->guests[req->gid];
  p = &g->proto;
  
  /* Get uninitialised queue from protocol list */
  q = &p->dqueues[req->qid];
  q->id = req->qid;

  /* Initialise dequeue struct */
  q->dq.head = 0;
  q->dq.nelems = req->nelems;
  q->dq.elsize = req->elsize;
  q->dq.off = req->off;
  q->dq.entries = g->shm_base + req->off;
  
  /* Add queue to protocol list */
  if (p->dqueues_tail == PROTOQ_ID_INVALID)
    p->dqueues_head = req->qid;
  else
    p->dqueues[p->dqueues_tail].next = req->qid;

  q->prev = p->dqueues_tail;
  q->next = PROTOQ_ID_INVALID;
  p->dqueues_tail = req->qid;
  p->ndqueues++;
  
  LOG_DEBUG("enabled queue qid=%d in core=%d", req->qid, req->core);
}

static void handle_disableq(struct fast_context *ctx, struct queue_entry *qe)
{
  struct guest_fast *g;
  struct proto_fast *p;
  struct queue_disableq_req *req;
  struct cham_dqueue *q;

  req = &qe->data.disableq_req;
  g = &ctx->guests[req->gid];
  p = &g->proto;
  
  q = &p->dqueues[req->qid];

  if (p->dqueues_head == q->id)
    p->dqueues_head = q->next;

  if (p->dqueues_tail == q->id)
    p->dqueues_tail = q->prev;

  if (q->next != PROTOQ_ID_INVALID)
    p->dqueues[q->next].prev = PROTOQ_ID_INVALID;

  if (q->prev != PROTOQ_ID_INVALID)
    p->dqueues[q->prev].next = PROTOQ_ID_INVALID;

  q->next = PROTOQ_ID_INVALID;
  q->prev = PROTOQ_ID_INVALID;
  p->ndqueues--;
}

static void handle_upload_ebpf(struct fast_context *ctx, struct queue_entry *qe)
{
  struct guest_fast *g;
  struct proto_fast *p;
  struct queue_up_ebpf_req *req;

  req = &qe->data.up_ebpf_req;
  g = &ctx->guests[req->gid];
  p = &g->proto;
  
  p->event_rx_vm = req->event_rx_vm;
  p->event_tx_vm = req->event_tx_vm;
  p->event_deq_vm = req->event_deq_vm;
}

static void handle_arp_tx_pkt(struct fast_context *ctx, struct queue_entry *qe)
{
  int ret;
  struct queue_entry *txqe;
  struct rte_mbuf **mb;
  
  /* Get packet from txq */
  txqe = queue_head(ctx->ctl_txq);
  if (txqe == NULL)
  {
    LOG_ERROR("failed to get head of control tx queue");
    return;
  } 
  
  /* TODO: Might want to look into doing this allocation in bulk */
  /* Allocate mbuf for transmission */
  txcache_alloc(ctx, &mb, 1);
  
  /* Copy packet data from shared memory to mbuf */
  mb[0]->data_off = 0;
  mb[0]->pkt_len = mb[0]->data_len = sizeof(struct pkt_arp);
  rte_memcpy(rte_pktmbuf_mtod(mb[0], __u8 *), &txqe->data, mb[0]->data_len);
  
  /* Add packet to transmit buffer */
  ctx->tx_mbs[ctx->tx_n] = mb[0];
  ctx->tx_n++;

/* Dequeue control tx queue */
  ret = queue_dequeue(ctx->ctl_txq);
  if (ret != 0)
  {
    LOG_ERROR("failed to dequeue control tx queue");
    return;
  }
}

static void handle_arp_update(struct fast_context *ctx, struct queue_entry *qe)
{
  int ret;
  struct queue_arp_update *arp_up = &qe->data.arp_update;
  
  ret = arp_insert(&ctx->arp_table, arp_up->ip, arp_up->mac);
  if (ret != 0)
  {
    LOG_ERROR("failed to insert new entry into ARP table");
    return;
  }
}
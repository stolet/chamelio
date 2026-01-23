#include <assert.h>
#include <linux/types.h>
#include <string.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include "control_ebpf.h"
#include "control_jit.h"
#include "ebpf.h"
#include "ebpf_jit.h"
#include "log.h"
#include "queue_fns.h"
#include "verifier.h"

void control_ebpf_allocate(struct guest_control *g,
    struct queue_entry *qe_req)
{
  int ret;
  struct queue_allocate_ebpf_req *req;
  struct queue_entry *qe_res;
  struct queue_allocate_ebpf_res *res;

  req = &qe_req->data.alloc_ebpf_req;
  qe_res = queue_tail(g->cham_guest_q);
  assert(qe_res != NULL);
  res = (struct queue_allocate_ebpf_res *)&qe_res->data;

  ret = shmalloc_alloc(g->alloc, req->size, &g->ebpf_shm_handle);
  if (ret != 0)
  {
    LOG_ERROR("failed to allocate memory for eBPF program");
    res->size = 0;
    res->off = 0;
    res->opaque = req->opaque;
    ret = queue_enqueue(g->cham_guest_q, QUEUE_ALLOCATE_EBPF_RES);
    assert(ret == 0);
    return;
  }

  memset(g->ebpf_shm_handle->addr, 0, g->ebpf_shm_handle->len);
  res->size = req->size;
  res->off = g->ebpf_shm_handle->off;
  res->opaque = req->opaque;
  ret = queue_enqueue(g->cham_guest_q, QUEUE_ALLOCATE_EBPF_RES);
  assert(ret == 0);
}

void control_ebpf_upload(struct control_context *ctx,
    struct guest_control *g, struct queue_entry *qe_req)
{
  int ret, i;
  void *ebpf_bytecode;
  struct queue_up_ebpf_req *req, *f_req;
  struct queue_up_ebpf_res *res;
  struct queue_entry *qe_res;
  struct bpf_object *bpf_obj;
  const void *event_rx_insns, *event_tx_insns, *event_deq_insns;
  struct bpf_program *event_rx_prog, *event_tx_prog, *event_deq_prog;
  struct ebpf_vm_c *event_rx_vm, *event_tx_vm, *event_deq_vm;
  const char *rx_entry = NULL;
  const char *tx_entry = NULL;
  const char *deq_entry = NULL;

  req = &qe_req->data.up_ebpf_req;

  qe_res = queue_tail(g->cham_guest_q);
  assert(qe_res != NULL);
  res = (struct queue_up_ebpf_res *)&qe_res->data;

  ebpf_bytecode = (__u8 *)g->shm_base + req->off;
  bpf_obj = bpf_object__open_mem(ebpf_bytecode, req->size, NULL);
  if (bpf_obj == NULL)
  {
    LOG_ERROR("failed to open bpf_obj from bytecode");
    res->success = 0;
    ret = queue_enqueue(g->cham_guest_q, QUEUE_UPLOAD_EBPF_RES);
    assert(ret == 0);
    return;
  }

  /* Verify and JIT RX snippet */
  event_rx_prog = bpf_object__find_program_by_name(bpf_obj, "event_rx");
  if (event_rx_prog == NULL)
  {
    LOG_ERROR("failed to get event_rx from bpf_obj");
    res->success = 0;
    ret = queue_enqueue(g->cham_guest_q, QUEUE_UPLOAD_EBPF_RES);
    assert(ret == 0);
    return;
  }

  event_rx_insns = bpf_program__insns(event_rx_prog);
  ret = verifier_analyze(event_rx_insns, bpf_program__insn_cnt(event_rx_prog),
      ctx->config->shm_len, "event_rx");
  if (ret != 0)
  {
    LOG_ERROR("failed to verify event_rx");
    return;
  }

  if (ctx->config->fp_jit_combined)
    rx_entry = EBPF_JIT_RX_STR;
  event_rx_vm = control_jit(ctx, event_rx_insns,
      bpf_program__insn_cnt(event_rx_prog) * 8, rx_entry);
  if (event_rx_vm == NULL)
  {
    LOG_ERROR("failed to jit event_rx");
    res->success = 0;
    ret = queue_enqueue(g->cham_guest_q, QUEUE_UPLOAD_EBPF_RES);
    assert(ret == 0);
    return;
  }

  /* Verify and JIT TX snippet */
  event_tx_prog = bpf_object__find_program_by_name(bpf_obj, "event_tx");
  if (event_rx_prog == NULL)
  {
    LOG_ERROR("failed to get event_tx from bpf_obj");
    res->success = 0;
    ret = queue_enqueue(g->cham_guest_q, QUEUE_UPLOAD_EBPF_RES);
    assert(ret == 0);
    return;
  }

  event_tx_insns = bpf_program__insns(event_tx_prog);
  ret = verifier_analyze(event_tx_insns, bpf_program__insn_cnt(event_tx_prog),
      ctx->config->shm_len, "event_tx");
  if (ret != 0)
  {
    LOG_ERROR("failed to verify event_tx");
    return;
  }

  if (ctx->config->fp_jit_combined)
    tx_entry = EBPF_JIT_TX_STR;
  event_tx_vm = control_jit(ctx, event_tx_insns,
      bpf_program__insn_cnt(event_tx_prog) * 8, tx_entry);
  if (event_tx_vm == NULL)
  {
    LOG_ERROR("failed to jit event_tx");
    res->success = 0;
    ret = queue_enqueue(g->cham_guest_q, QUEUE_UPLOAD_EBPF_RES);
    assert(ret == 0);
    return;
  }

  /* Verify and JIT DEQ snippet */
  event_deq_prog = bpf_object__find_program_by_name(bpf_obj, "event_deq");
  if (event_rx_prog == NULL)
  {
    LOG_ERROR("failed to get event_deq from bpf_obj");
    res->success = 0;
    ret = queue_enqueue(g->cham_guest_q, QUEUE_UPLOAD_EBPF_RES);
    assert(ret == 0);
    return;
  }

  event_deq_insns = bpf_program__insns(event_deq_prog);
  ret = verifier_analyze(event_deq_insns, bpf_program__insn_cnt(event_deq_prog),
      ctx->config->shm_len, "event_deq");
  if (ret != 0)
  {
    LOG_ERROR("failed to verify event_deq");
    return;
  }
  if (ctx->config->fp_jit_combined)
    deq_entry = EBPF_JIT_DEQ_STR;
  event_deq_vm = control_jit(ctx, event_deq_insns,
      bpf_program__insn_cnt(event_deq_prog) * 8, deq_entry);
  if (event_deq_vm == NULL)
  {
    LOG_ERROR("failed to jit event_deq");
    res->success = 0;
    ret = queue_enqueue(g->cham_guest_q, QUEUE_UPLOAD_EBPF_RES);
    assert(ret == 0);
    return;
  }

  /* Send jitted VMs for functions to fast-path  */
  for (i = 0; i < ctx->config->fp_cores_max; i++)
  {
    qe_req = queue_tail(ctx->ctl_fast_qs[i]);
    assert(qe_req != NULL);
    f_req = &qe_req->data.up_ebpf_req;
    f_req->gid = g->id;
    f_req->size = req->size;
    f_req->off = req->off;
    f_req->event_rx_vm = event_rx_vm;
    f_req->event_tx_vm = event_tx_vm;
    f_req->event_deq_vm = event_deq_vm;
    ret = queue_enqueue(ctx->ctl_fast_qs[i], QUEUE_UPLOAD_EBPF_REQ);
    assert(ret == 0);
  }

  res = (struct queue_up_ebpf_res *)&qe_res->data;
  res->success = 1;
  ret = queue_enqueue(g->cham_guest_q, QUEUE_UPLOAD_EBPF_RES);
  assert(ret == 0);
}

void control_ebpf_free(struct guest_control *g,
    struct queue_entry *qe_req)
{
  int ret;
  struct queue_free_ebpf_res *res;
  struct queue_entry *qe_res;

  qe_res = queue_tail(g->cham_guest_q);
  assert(qe_res != NULL);

  res = (struct queue_free_ebpf_res *)&qe_res->data;

  shmalloc_free(g->alloc, g->ebpf_shm_handle);

  res->success = 1;
  ret = queue_enqueue(g->cham_guest_q, QUEUE_FREE_EBPF_RES);
  assert(ret == 0);
}

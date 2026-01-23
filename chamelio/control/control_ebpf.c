#include <assert.h>
#include <linux/types.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include <rte_ip4.h>

#include "control_ebpf.h"
#include "clang.h"
#include "ebpf.h"
#include "log.h"
#include "queue_fns.h"
#include "scheduler_fns.h"
#include "verifier.h"

/* Combined infra + ebpf entry symbols in the bytecode. */
#define COMB_RX_ENTRY "fast_comb_rx"
#define COMB_TX_ENTRY "fast_comb_tx"
#define COMB_DEQ_ENTRY "fast_comb_deq"

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

static struct ebpf_vm_c *verify_and_jit(struct control_context *ctx,
    struct bpf_object *bpf_obj, const char *prog_name, const char *comb_entry);
static struct ebpf_vm_c *jit(struct control_context *ctx,
    const void *ebpf_instrs, size_t size, const char *comb_entry);
static int load_comb_bytecode(struct control_context *ctx);
static int register_helpers(struct ebpf_vm_c *vm);

/* Helpers */
static void ebpf_print(int a);
static inline void *ebpf_memcpy(void *dst, void *src, size_t n);
static inline __u16 ebpf_ipv4_checksum(void *ip_hdr);
static inline __u16 ebpf_ipv4_udptcp_cksum(void *ip_hdr, void *udp_hdr);
static inline void *ebpf_map_get(void *map_base, __u32 len);
static inline void *ebpf_map_lookup(void *map_base, __u64 id, __u64 elsize);
static inline void *ebpf_queue_tail(struct equeue *q, __u64 elsize);

enum ebpf_helper_id
{
  EBPF_HELPER_QUEUE_TAIL = 1001,
  EBPF_HELPER_QUEUE_ENQUEUE = 1002,
  EBPF_HELPER_MEMCPY = 1003,
  EBPF_HELPER_PRINT = 1004,
  EBPF_HELPER_IPV4_CKSUM = 1005,
  EBPF_HELPER_IPV4_UDPTCP_CKSUM = 1006,
  EBPF_HELPER_SCHED_HEAD = 1007,
  EBPF_HELPER_SCHED_POP = 1008,
  EBPF_HELPER_SCHED_ADD = 1009,
  EBPF_HELPER_MAP_GET = 1010,
  EBPF_HELPER_MAP_LOOKUP = 1011,
};

struct ebpf_helper_desc
{
  __u32 id;
  const char *name;
  void *fn;
};

struct ebpf_prog_desc
{
  const char *prog_name;
  const char *comb_entry;
  struct ebpf_vm_c **out_vm;
};

static const struct ebpf_helper_desc ebpf_helpers[] = {
  { EBPF_HELPER_QUEUE_TAIL, 
      "ebpf_queue_tail", ebpf_queue_tail },
  { EBPF_HELPER_QUEUE_ENQUEUE, 
      "queue_enqueue", queue_enqueue },
  { EBPF_HELPER_MEMCPY, 
      "ebpf_memcpy", ebpf_memcpy },
  { EBPF_HELPER_PRINT, 
      "ebpf_print", ebpf_print },
  { EBPF_HELPER_IPV4_CKSUM, 
      "ebpf_ipv4_checksum", ebpf_ipv4_checksum },
  { EBPF_HELPER_IPV4_UDPTCP_CKSUM, 
    "ebpf_ipv4_udptcp_cksum", ebpf_ipv4_udptcp_cksum },
  { EBPF_HELPER_SCHED_HEAD, 
      "sched_head", sched_head },
  { EBPF_HELPER_SCHED_POP, 
      "sched_pop", sched_pop },
  { EBPF_HELPER_SCHED_ADD, 
      "sched_add", sched_add },
  { EBPF_HELPER_MAP_GET, 
      "ebpf_map_get", ebpf_map_get },
  { EBPF_HELPER_MAP_LOOKUP, 
      "ebpf_map_lookup", ebpf_map_lookup },
};

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
  int success = 0;
  void *ebpf_bytecode;
  struct queue_up_ebpf_req *req, *f_req;
  struct queue_up_ebpf_res *res;
  struct queue_entry *qe_res;
  struct bpf_object *bpf_obj = NULL;
  struct ebpf_vm_c *event_rx_vm = NULL;
  struct ebpf_vm_c *event_tx_vm = NULL;
  struct ebpf_vm_c *event_deq_vm = NULL;
  struct ebpf_prog_desc progs[] = {
      { "event_rx", COMB_RX_ENTRY, &event_rx_vm },
      { "event_tx", COMB_TX_ENTRY, &event_tx_vm },
      { "event_deq", COMB_DEQ_ENTRY, &event_deq_vm },
  };

  req = &qe_req->data.up_ebpf_req;

  qe_res = queue_tail(g->cham_guest_q);
  assert(qe_res != NULL);
  res = (struct queue_up_ebpf_res *)&qe_res->data;

  ebpf_bytecode = (__u8 *) g->shm_base + req->off;
  bpf_obj = bpf_object__open_mem(ebpf_bytecode, req->size, NULL);
  if (bpf_obj == NULL)
  {
    LOG_ERROR("failed to open bpf_obj from bytecode");
    goto out;
  }

  for (i = 0; i < ARRAY_SIZE(progs); i++)
  {
    *progs[i].out_vm = verify_and_jit(ctx, bpf_obj,
        progs[i].prog_name, progs[i].comb_entry);
    if (*progs[i].out_vm == NULL)
      goto out;
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

  success = 1;

out:
  if (!success)
  {
    if (event_rx_vm != NULL)
      ebpf_vm_destroy(event_rx_vm);
    if (event_tx_vm != NULL)
      ebpf_vm_destroy(event_tx_vm);
    if (event_deq_vm != NULL)
      ebpf_vm_destroy(event_deq_vm);
  }

  if (bpf_obj != NULL)
    bpf_object__close(bpf_obj);

  res->success = success;
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

static struct ebpf_vm_c *verify_and_jit(
    struct control_context *ctx, struct bpf_object *bpf_obj,
    const char *prog_name, const char *comb_entry)
{
  int ret;
  const void *insns;
  struct bpf_program *prog;
  struct ebpf_vm_c *vm;

  prog = bpf_object__find_program_by_name(bpf_obj, prog_name);
  if (prog == NULL)
  {
    LOG_ERROR("failed to get %s from bpf_obj", prog_name);
    return NULL;
  }

  insns = bpf_program__insns(prog);
  ret = verifier_analyze(insns, bpf_program__insn_cnt(prog),
      ctx->config->shm_len, (char *)prog_name);
  if (ret != 0)
  {
    LOG_ERROR("failed to verify %s", prog_name);
    return NULL;
  }

  vm = jit(ctx, insns, bpf_program__insn_cnt(prog) * 8, comb_entry);
  if (vm == NULL)
    LOG_ERROR("failed to jit %s", prog_name);

  return vm;
}

static struct ebpf_vm_c *jit(struct control_context *ctx,
    const void *ebpf_instrs, size_t size, const char *comb_entry)
{
  __u64 res;
  struct ebpf_vm_c *vm;

  vm = ebpf_vm_create();
  if (vm == NULL)
  {
    LOG_ERROR("failed to create llvmbpf vm");
    return NULL;
  }

  /* Load eBPF instructions into llvmbpf vm */
  res = ebpf_vm_load_code(vm, ebpf_instrs, size);
  if (res != 0)
  {
    LOG_ERROR("failed to load ebpf bytecode");
    goto error;
  }

  /* Registers the helper functions used by the eBPF snippets */
  res = register_helpers(vm);
  if (res != 0)
  {
    LOG_ERROR("failed to register helpers");
    goto error;
  }

  /* Compile combined infra + ebpf */
  if (ctx->config->fp_jit_combined)
  {
    /* Load and compile infrastructure code */
    res = load_comb_bytecode(ctx);
    if (res != 0)
    {
      LOG_ERROR("failed to build infra bytecode");
      goto error;
    }

    /* Get a combined bytecode of the infra code + ebpf snippets */
    res = ebpf_vm_compile_combined(vm, ctx->infra_bc.data,
        ctx->infra_bc.len, comb_entry);
    if (res != 0)
    {
      LOG_ERROR("failed to compile combined infra + ebpf module");
      goto error;
    }

    return vm;
  }

  /* Compile just eBPF snippet if combined jit not turned on */
  res = ebpf_vm_compile(vm);
  if (res != 0)
  {
    LOG_ERROR("failed to JIT ebpf bytecode");
    goto error;
  }

  return vm;

error:
  ebpf_vm_destroy(vm);
  return NULL;
}

static int register_helpers(struct ebpf_vm_c *vm)
{
  int res;
  size_t i;

  for (i = 0; i < ARRAY_SIZE(ebpf_helpers); i++)
  {
    res = ebpf_vm_register_helper(vm, ebpf_helpers[i].id,
        ebpf_helpers[i].name, ebpf_helpers[i].fn);
    if (res != 0)
    {
      LOG_ERROR("failed to register %s helper", ebpf_helpers[i].name);
      return -1;
    }
  }

  return 0;
}

static int load_comb_bytecode(struct control_context *ctx)
{
  int ret;
  char src_path[PATH_MAX];

  /* These are passed as preprocessor macros during compilation */
  const char *build_dir = CHAMELIO_BUILD_DIR;
  const char *src_dir = CHAMELIO_SRC_DIR;

  if (ctx->infra_bc.data != NULL)
  {
    LOG_ERROR("Already loaded infra bytecode");
    return 0;
  }

  ret = snprintf(src_path, sizeof(src_path),
      "%s/chamelio/fast/fast_comb.c", src_dir);
  if (ret < 0 || ret >= (int)sizeof(src_path))
  {
    LOG_ERROR("failed to build infra source path");
    return -1;
  }

  return clang_compile(build_dir, src_path,
      &ctx->infra_bc.data, &ctx->infra_bc.len);
}

static void ebpf_print(int a)
{
  LOG_DEBUG("ebpf_print %d", a);
}

static inline void *ebpf_memcpy(void *dst, void *src, size_t n)
{
  return memcpy(dst, src, n);
}

static inline __u16 ebpf_ipv4_checksum(void *ip_hdr)
{
  return rte_ipv4_cksum(ip_hdr);
}

static inline __u16 ebpf_ipv4_udptcp_cksum(void *ip_hdr, void *udp_hdr)
{
  return rte_ipv4_udptcp_cksum(ip_hdr, udp_hdr);
}

static inline void *ebpf_map_get(void *map_base, __u32 len)
{
  return map_base;
}

static inline void *ebpf_map_lookup(void *map_base, __u64 id, __u64 elsize)
{
  return (__u8 *)map_base + (id * elsize);
}

static inline void *ebpf_queue_tail(struct equeue *q, __u64 elsize)
{
  return queue_tail(q);
}

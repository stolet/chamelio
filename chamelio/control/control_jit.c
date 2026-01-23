#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

#include <rte_ip4.h>

#include "control.h"
#include "scheduler_fns.h"
#include "queue_fns.h"
#include "ebpf.h"
#include "log.h"
#include "clang.h"

static int load_infra(struct control_context *ctx);
static int register_helpers(struct ebpf_vm_c *vm);

static void ebpf_print(int a);
static inline void * ebpf_memcpy(void *dst, void *src, size_t n);
static inline __u16 ebpf_ipv4_checksum(void *ip_hdr);
static inline __u16 ebpf_ipv4_udptcp_cksum(void *ip_hdr, void *udp_hdr);
static inline void * ebpf_map_get(void *map_base, __u32 len);
static inline void * ebpf_map_lookup(void *map_base, __u64 id, __u64 elsize);
static inline void * ebpf_queue_tail(struct equeue *q, __u64 elsize);

struct ebpf_vm_c * control_jit(struct control_context *ctx,
    const void *ebpf_instrs, size_t size, const char *entry_sym)
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
    return NULL;
  }

  /* Registers the helper functions used by the eBPF snippets */
  res = register_helpers(vm);
  if (res != 0)
  {
    LOG_ERROR("failed to register helpers");
    return NULL;
  }

  /* Compile combined infra + ebpf else just ebpf */
  if (ctx->config->fp_jit_combined)
  {

    /* Load and compile infrastructure code */
    res = load_infra(ctx);
    if (res != 0)
    {
      LOG_ERROR("failed to build infra bytecode");
      return NULL;
    }

    /* Get a combined bytecode of the infra code + ebpf snippets */
    res = ebpf_vm_compile_combined(vm, ctx->infra_bc.data,
        ctx->infra_bc.len, entry_sym);
    if (res != 0)
    {
      LOG_ERROR("failed to compile combined infra + ebpf module");
      return NULL;
    }
    
    return vm;
  }

  /* Compile just eBPF snippet if combined jit not turned on */
  res = ebpf_vm_compile(vm);
  if (res != 0)
  {
    LOG_ERROR("failed to JIT ebpf bytecode");
    return NULL;
  }

  return vm;
}

static int register_helpers(struct ebpf_vm_c *vm)
{
  int res;
  
  res = ebpf_vm_register_helper(vm, 1001, "ebpf_queue_tail", ebpf_queue_tail);
  if (res != 0)
  {
    LOG_ERROR("failed to register ebpf_queue_tail helper");
    return -1;
  }
  
  res = ebpf_vm_register_helper(vm, 1002, "queue_enqueue", queue_enqueue);
  if (res != 0)
  {
    LOG_ERROR("failed to register ebpf_queue_enqueue helper");
    return -1;  
  }
  
  res = ebpf_vm_register_helper(vm, 1003, "ebpf_memcpy", ebpf_memcpy);
  if (res != 0)
  {
    LOG_ERROR("failed to register ebpf_memcpy helper");
    return -1;
  }

  res = ebpf_vm_register_helper(vm, 1004, "ebpf_print", ebpf_print);
  if (res != 0)
  {
    LOG_ERROR("failed to register ebpf_print helper");
    return -1;
  }

  res = ebpf_vm_register_helper(vm, 1005, "ebpf_ipv4_checksum", ebpf_ipv4_checksum);
  if (res != 0)
  {
    LOG_ERROR("failed to register ebpf_ipv4_checksum helper");
    return -1;
  }
  
  res = ebpf_vm_register_helper(vm, 1006, "ebpf_ipv4_udptcp_cksum", ebpf_ipv4_udptcp_cksum);
  if (res != 0)
  {
    LOG_ERROR("failed to register ebpf_ipv4_udptcp_cksum helper");
    return -1;
  }
  
  res = ebpf_vm_register_helper(vm, 1007, "sched_head", sched_head);
  if (res != 0)
  {
    LOG_ERROR("failed to register sched_head helper");
    return -1;
  }
  
  res = ebpf_vm_register_helper(vm, 1008, "sched_pop", sched_pop);
  if (res != 0)
  {
    LOG_ERROR("failed to register sched_pop helper");
    return -1;
  }
  
  res = ebpf_vm_register_helper(vm, 1009, "sched_add", sched_add);
  if (res != 0)
  {
    LOG_ERROR("failed to register sched_add helper");
    return -1;
  }

  res = ebpf_vm_register_helper(vm, 1010, "ebpf_map_get", ebpf_map_get);
  if (res != 0)
  {
    LOG_ERROR("failed to register ebpf_map_get helper");
    return -1;
  }

  res = ebpf_vm_register_helper(vm, 1011, "ebpf_map_lookup", ebpf_map_lookup);
  if (res != 0)
  {
    LOG_ERROR("failed to register ebpf_map_lookup helper");
    return -1;
  }

  return 0;
}

static int load_infra(struct control_context *ctx)
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
      "%s/chamelio/fast/fast_jit.c", src_dir);
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
  LOG_DEBUG("HERE %lld", a);
}

static inline void * ebpf_memcpy(void *dst, void *src, size_t n)
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

static inline void * ebpf_map_get(void *map_base, __u32 len)
{
  return map_base;
}

static inline void * ebpf_map_lookup(void *map_base, __u64 id, __u64 elsize)
{
  return map_base + (id * elsize);
}

static inline void * ebpf_queue_tail(struct equeue *q, __u64 elsize)
{
  return queue_tail(q);
}
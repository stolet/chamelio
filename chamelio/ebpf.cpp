// llvmbpf_shim.cpp
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>
#include <iostream>

#include <llvmbpf/llvmbpf.hpp>
#include "ebpf.h"
using namespace bpftime; 

extern "C"
{
  /* eBPF entry symbol used to call combined JIT modules */
  #define EBPF_JIT_ENTRY_STR "__cham_comb"
  
  struct ebpf_vm_c
  {
    llvmbpf_vm vm;
    ebpf_jitted_fn jitted_fn = nullptr;
    
  };

  ebpf_vm_c *ebpf_vm_create(void)
  {
    try
    {
      return new ebpf_vm_c{};
    }
    catch (...)
    {
      return nullptr;
    }
  }

  void ebpf_vm_destroy(ebpf_vm_c *h)
  {
    delete h;
  }

  int ebpf_vm_load_code(ebpf_vm_c *h, const void *code, size_t code_len)
  {
    int rc;
    if (!h || !code)
      return -1;
    rc = h->vm.load_code(code, code_len);
    return rc == 0 ? 0 : rc;
  }

  void ebpf_vm_unload_code(ebpf_vm_c *h)
  {
    if (!h)
      return;
    h->vm.unload_code();
    h->jitted_fn = nullptr;
  }

  int ebpf_vm_register_helper(ebpf_vm_c *h, __u32 id, const char *name,
      void *fn)
  {
    int res; 
    if (!h || !fn)
      return -1;
    res = h->vm.register_external_function(id, name, fn);
    return res;
  }

  // return NULLPTR on error and function pointer to the jitted code on success
  int ebpf_vm_compile(ebpf_vm_c *h)
  {
    if (!h)
      return -1;
    
    auto f = h->vm.compile();
    if (!f.has_value())
      return -1;
    
    h->jitted_fn = f.value();
    return 0;
  }

  int ebpf_vm_compile_combined(ebpf_vm_c *h, const void *code,
      size_t code_len, const char *entry_sym)
  {
    if (!h || !code || !entry_sym)
      return -1;

    const auto *bc_bytes = reinterpret_cast<const uint8_t *>(code);
    std::vector<uint8_t> bc_vec(bc_bytes, bc_bytes + code_len);
    auto f = h->vm.compile_with_external_bitcode(
        bc_vec, entry_sym, EBPF_JIT_ENTRY_STR);
    
    if (!f.has_value())
      return -1;

    h->jitted_fn = f.value();
    return 0;
  }

  int ebpf_vm_emit_named_bitcode(ebpf_vm_c *h, const char *func_name,
      void **out_data, size_t *out_len)
  {
    void *buf;

    if (!h || !func_name || !out_data || !out_len)
      return -1;

    auto bc = h->vm.emit_bitcode(func_name);
    if (!bc.has_value())
      return -1;

    buf = malloc(bc->size());
    if (buf == nullptr)
      return -1;

    memcpy(buf, bc->data(), bc->size());
    *out_data = buf;
    *out_len = bc->size();
    return 0;
  }

  int ebpf_vm_compile_bitcode_modules(ebpf_vm_c *h,
      const void *const *mods, const size_t *mods_len, size_t nr_mods,
      const char *entry_sym)
  {
    std::vector<std::vector<uint8_t>> bc_mods;

    if (!h || !mods || !mods_len || !entry_sym || nr_mods == 0)
      return -1;

    bc_mods.reserve(nr_mods);
    for (size_t i = 0; i < nr_mods; i++)
    {
      const auto *bc_bytes = reinterpret_cast<const uint8_t *>(mods[i]);
      bc_mods.emplace_back(bc_bytes, bc_bytes + mods_len[i]);
    }

    auto f = h->vm.compile_with_bitcode_modules(bc_mods, entry_sym);
    if (!f.has_value())
      return -1;

    h->jitted_fn = f.value();
    return 0;
  }

  ebpf_jitted_fn ebpf_vm_jitted_fn(ebpf_vm_c *h)
  {
    if (!h)
      return nullptr;
    return h->jitted_fn;
  }

  int ebpf_vm_exec(ebpf_vm_c *h, void *arg,
      size_t arg_len, int *return_value)
  {
    int exec_ret;
    uint64_t rv = 0;
    
    exec_ret = h->vm.exec(arg, arg_len, rv);
    *return_value = rv;
    
    return exec_ret;
  }

} 

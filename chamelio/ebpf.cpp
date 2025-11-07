// llvmbpf_shim.cpp
#include <cstdint>
#include <cstddef>
#include <new>

#include <llvmbpf/llvmbpf.hpp>
#include "ebpf.h"
using namespace bpftime; 

extern "C"
{


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

  int ebpf_vm_register_helper(ebpf_vm_c *h, __u32 id, void *fn, char *name)
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

  int ebpf_vm_exec(ebpf_vm_c *h, void *arg, 
      size_t arg_len, __u64 *return_value)
  {
    uint64_t rv = return_value ? *return_value : 0;
    return h->vm.exec(arg, arg_len, rv);
    
    if (return_value)
      *return_value = rv;
  }

} 

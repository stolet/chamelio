#include <cstdint>
#include <cstddef>
#include <new>
#include <string.h>
#include <iostream>

#include <llvmbpf/llvmbpf.hpp>
#include "ebpfif.h"
using namespace bpftime; 

extern "C"
{


  struct llvmbpf_vm_c
  {
    llvmbpf_vm vm;
    llvmbpf_jitted_fn jitted_fn_rx = nullptr;
    llvmbpf_jitted_fn jitted_fn_tx = nullptr;
    llvmbpf_jitted_fn jitted_fn_deq = nullptr;
  };

  llvmbpf_vm_c *llvmbpf_vm_create(void)
  {
    try
    {
      return new llvmbpf_vm_c{};
    }
    catch (...)
    {
      return nullptr;
    }
  }

  void llvmbpf_vm_destroy(llvmbpf_vm_c *h)
  {
    delete h;
  }

  int llvmbpf_vm_load_code(llvmbpf_vm_c *h, const void *code, size_t code_len)
  {
    int rc;
    if (!h || !code)
      return -1;
    rc = h->vm.load_code(code, code_len);
    return rc == 0 ? 0 : rc;
  }

  void llvmbpf_vm_unload_code(llvmbpf_vm_c *h)
  {
    if (!h)
      return;
    h->vm.unload_code();
    h->jitted_fn_rx = nullptr;
    h->jitted_fn_tx = nullptr;
    h-> jitted_fn_deq = nullptr;
  }

  int llvmbpf_vm_register_helper(llvmbpf_vm_c *h, uint32_t id, void *fn, char *name)
  {
    int res; 
    if (!h || !fn)
      return -1;
    res = h->vm.register_external_function(id, name, fn);
    return res;
  }

  // return 0 upon successful compilation
  int llvmbpf_vm_compile(llvmbpf_vm_c *h, const char *prog_name)
  {
    if (!h)
      return -1;
    auto f = h->vm.compile();
    if (!f.has_value())
      return -1;
    if (strcmp(prog_name, "prog/udp_rx") == 0)
      h->jitted_fn_rx = f.value();
    else if (strcmp(prog_name, "prog/udp_tx") == 0)
      h->jitted_fn_tx = f.value();
    else if (strcmp(prog_name, "prog/udp_deq") == 0)
      h->jitted_fn_deq = f.value();
    else
      printf("Unknown eBPF program name: %s\n", prog_name);
    return 0;
  }

} 

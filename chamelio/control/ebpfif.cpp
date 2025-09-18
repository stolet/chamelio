// llvmbpf_shim.cpp
#include <cstdint>
#include <cstddef>
#include <new>

#include <llvmbpf/llvmbpf.hpp>
#include "ebpfif.h"
using namespace bpftime; // llvmbpf_vm lives here in the project

extern "C"
{

  // Keep the C++ object in an opaque C struct
  struct llvmbpf_vm_c
  {
    llvmbpf_vm vm;
    llvmbpf_jitted_fn jitted_fn = nullptr;
    // add field to retain the compiled function pointer if needed
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
    if (!h || !code)
      return -1;
    int rc = h->vm.load_code(code, code_len);
    return rc == 0 ? 0 : rc;
  }

  void llvmbpf_vm_unload_code(llvmbpf_vm_c *h)
  {
    if (!h)
      return;
    h->vm.unload_code();
    h->jitted_fn = nullptr;
  }

  int llvmbpf_vm_register_helper(llvmbpf_vm_c *h, uint32_t id, void *fn, char *name)
  {
    int res; 
    if (!h || !fn)
      return -1;
    res = h->vm.register_external_function(id, name, fn);
    return res;
  }

  // return NULLPTR on error and function pointer to the jitted code on success
  int llvmbpf_vm_compile(llvmbpf_vm_c *h)
  {
    if (!h)
      return -1;
    auto f = h->vm.compile();
    if (!f.has_value())
      return -1;
    h->jitted_fn = f.value();
    return 0;
  }

} // extern "C"

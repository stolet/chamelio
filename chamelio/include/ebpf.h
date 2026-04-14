#pragma once
#include <stddef.h>
#include <stdint.h>
#include <linux/types.h>

#ifdef __cplusplus
extern "C"
{
#endif

  /* Handle to the C++ VM */
  typedef struct ebpf_vm_c ebpf_vm_c;

  /* JITed function signature */
  #ifndef EBPF_JITTED_FN_T
  #define EBPF_JITTED_FN_T
  typedef uint64_t (*ebpf_jitted_fn)(void *mem, size_t mem_len);
  #endif

  /* Create/destroy VM */
  ebpf_vm_c *ebpf_vm_create(void);
  void ebpf_vm_destroy(ebpf_vm_c *h);

  /* Load/unload eBPF bytecode */
  int ebpf_vm_load_code(ebpf_vm_c *h, const void *code, size_t code_len);
  void ebpf_vm_unload_code(ebpf_vm_c *h);

  /* Register helper functions */
  int ebpf_vm_register_helper(ebpf_vm_c *h, __u32 id, const char *name,
    void *fn);

  /* JIT compiled loaded eBPF code */
  int ebpf_vm_compile(ebpf_vm_c *h);

  /* JIT compile with extra infra bytecode and a custom entry symbol */
  int ebpf_vm_compile_combined(ebpf_vm_c *h, const void *code,
    size_t code_len, const char *entry_sym);

  /* Emit LLVM bitcode for the loaded eBPF program with a custom symbol name */
  int ebpf_vm_emit_named_bitcode(ebpf_vm_c *h, const char *func_name,
    void **out_data, size_t *out_len);

  /* JIT compile an aggregate entry from a list of LLVM bitcode modules */
  int ebpf_vm_compile_bitcode_modules(ebpf_vm_c *h,
    const void *const *mods, const size_t *mods_len, size_t nr_mods,
    const char *entry_sym);

  /* Return the compiled function pointer */
  ebpf_jitted_fn ebpf_vm_jitted_fn(ebpf_vm_c *h);

  /* Execute jitted function */
  int ebpf_vm_exec(ebpf_vm_c *h, void *arg, 
    size_t arg_len, int *return_value);

#ifdef __cplusplus
}
#endif

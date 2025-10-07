#pragma once
#include <stddef.h>
#include <stdint.h>
//#include "bpf/libbpf.h"

#ifdef __cplusplus
extern "C"
{
#endif

    // handle to the C++ VM
    typedef struct llvmbpf_vm_c llvmbpf_vm_c;

    // JITed function signature
    typedef uint64_t (*llvmbpf_jitted_fn)(void *mem, size_t mem_len);

    // Create/destroy VM
    llvmbpf_vm_c *llvmbpf_vm_create(void);
    void llvmbpf_vm_destroy(llvmbpf_vm_c *h);

    // Load/unload eBPF bytecode
    int llvmbpf_vm_load_code(llvmbpf_vm_c *h, const void *code, size_t code_len);
    void llvmbpf_vm_unload_code(llvmbpf_vm_c *h);

    // Register helper functions
    int llvmbpf_vm_register_helper(llvmbpf_vm_c *h, uint32_t id, void *fn, char *name);

    /**
     * JIT compile loaded eBPF code
     * Returns 0 on success, -1 on error
     */
    int llvmbpf_vm_compile(llvmbpf_vm_c *h); // 0=ok

    llvmbpf_jitted_fn   llvmbpf_vm_get_jitted_fn(llvmbpf_vm_c* h);              // NULL if none

#ifdef __cplusplus
}
#endif

#pragma once
#include <stddef.h>
#include <stdint.h>
#include "bpf/libbpf.h"

#ifdef __cplusplus
extern "C"
{
#endif

    // handle to the C++ VM
    typedef struct llvmbpf_vm_c llvmbpf_vm_c;

    // JITed function signature
    typedef uint64_t (*llvmbpf_jitted_fn)(void *mem, size_t mem_len);

    llvmbpf_vm_c *llvmbpf_vm_create(void);
    void llvmbpf_vm_destroy(llvmbpf_vm_c *h);

    int llvmbpf_vm_load_code(llvmbpf_vm_c *h, const void *code, size_t code_len);
    void llvmbpf_vm_unload_code(llvmbpf_vm_c *h);

    int llvmbpf_vm_compile(llvmbpf_vm_c *h); // 0=ok

llvmbpf_jitted_fn   llvmbpf_vm_get_jitted_fn(llvmbpf_vm_c* h);              // NULL if none
/*
int                 llvmbpf_vm_exec(llvmbpf_vm_c* h,
                                    void* mem, size_t mem_len,
                                    uint64_t* bpf_ret_out);                 // 0=ok
*/
#ifdef __cplusplus
}
#endif

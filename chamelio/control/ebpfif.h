#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

    // handle to the C++ VM
    typedef struct llvmbpf_vm_c llvmbpf_vm_c;

    // JITed function pointer type
    typedef uint64_t (*llvmbpf_jitted_fn)(void *mem, size_t mem_len);

        // Create and destroy the VM handle
    llvmbpf_vm_c *llvmbpf_vm_create(void);
    void llvmbpf_vm_destroy(llvmbpf_vm_c *h);

    // Load and unload eBPF bytecode
    int llvmbpf_vm_load_code(llvmbpf_vm_c *h, const void *code, size_t code_len);
    void llvmbpf_vm_unload_code(llvmbpf_vm_c *h);
    int llvmbpf_vm_register_helper(llvmbpf_vm_c *h, uint32_t id, void *fn, char *name);

    int llvmbpf_vm_compile(llvmbpf_vm_c *h); // 0=ok
    //getter for the jitted function pointer
    llvmbpf_jitted_fn llvmbpf_vm_get_jitted_function(llvmbpf_vm_c *h);

#ifdef __cplusplus
}
#endif

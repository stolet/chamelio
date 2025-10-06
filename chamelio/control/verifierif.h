#pragma once 
#ifdef __cplusplus
extern "C"
{
#endif

//verify the provided eBPF bytecode
bool verify_ebpf_cham(void* raw_instr, size_t size);

#ifdef __cplusplus
}

#endif
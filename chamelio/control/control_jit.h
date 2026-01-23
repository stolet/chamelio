#ifndef CONTROL_JIT_H_
#define CONTROL_JIT_H_

#include <stdlib.h>

#include "control.h"
#include "ebpf.h"

struct ebpf_vm_c * control_jit(struct control_context *ctx,
    const void *ebpf_instrs, size_t size, const char *entry_sym);

#endif
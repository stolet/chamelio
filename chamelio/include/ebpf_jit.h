#ifndef EBPF_JIT_H_
#define EBPF_JIT_H_

/* eBPF entry symbol used in combined JIT modules. */
#define EBPF_JIT_ENTRY_STR "__chamelio_ebpf"

/* Combined infra entry symbols in the build-time bytecode. */
#define EBPF_JIT_RX_STR "fast_jit_rx"
#define EBPF_JIT_TX_STR "fast_jit_tx"
#define EBPF_JIT_DEQ_STR "fast_jit_deq"

#endif

#pragma once 

#ifdef __cplusplus
extern "C"
{
#endif

  int verifier_analyze(const void * raw_instr, size_t size,
      __u64 shm_len, __u32 perf_iso_max_ins, char *name);

#ifdef __cplusplus
}

#endif

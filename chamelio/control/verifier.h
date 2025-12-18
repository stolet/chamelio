#pragma once 

#ifdef __cplusplus
extern "C"
{
#endif

  int verifier_analyze(const void * raw_instr, size_t size, 
      __u64 shm_len, char *name);

#ifdef __cplusplus
}

#endif
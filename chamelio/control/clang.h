#ifndef CLANG_H_
#define CLANG_H_

#include <stddef.h>

int clang_compile(const char *build_dir, const char *cmd_src_path,
  const char *src_path, const char *const *extra_defs, size_t nr_defs,
  void **out_data, size_t *out_len);

#endif

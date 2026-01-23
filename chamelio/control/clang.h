#ifndef CLANG_H_
#define CLANG_H_

#include <stddef.h>

int clang_compile(const char *build_dir, const char *src_path,
  void **out_data, size_t *out_len);

#endif

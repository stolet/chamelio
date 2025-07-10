#ifndef FAST_H_
#define FAST_H_

#include <stdint.h>

struct fast_path_context {
  uint8_t id;
};

int fast_path_context_init(struct fast_path_context *ctx);
void fast_path_loop(struct fast_path_context *ctx);
void fast_path_context_destroy();

#endif
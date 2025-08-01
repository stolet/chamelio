#ifndef CHAM_LIB_H_
#define CHAM_LIB_H_

#include <stdint.h>

struct app_lib {
  int uxsocket_fd;
  int shm_fd;
  void *shm_base;
};

int cham_init_guest();
struct app_lib * cham_init_app();
int cham_init_app_ctx(struct app_lib *a, uint8_t proto_type);

#endif
#include <stdlib.h>
#include <cham_lib.h>
#include <cham_guest_lib.h>

int main (int argc, char **argv)
{
  struct guest_lib *g;
  struct proto_lib *p;

struct guest_lib * cham_connect_guest();
/* Creates a new protocol and maps shared memory region */
struct proto_lib* cham_new_proto(struct guest_lib *g, uint32_t shmsize);

  g = cham_connect_guest();
  if (g == NULL)
    abort();

  p = cham_new_proto(g, 0);
  if (p == NULL)
    abort();

  // struct app_lib *a;
  // struct app_context_lib *actx;
  // struct buf_lib *buf;

  // // cham_init_guest();
  // a = cham_init_app();
  // actx = cham_init_app_ctx(a, 0);
  // buf = cham_new_buf(actx);
  // if (buf == NULL)
  // {
  //   abort();
  // }

  // while (1) {
  //   cham_poll_slow(actx);
  //   cham_poll_bump(actx);
  // }
}